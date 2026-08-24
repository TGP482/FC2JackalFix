module;

#include <common.hxx>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <intrin.h>

export module utilisation;

import common;
import dunia;
import settings;

// Held back for the main and render threads, which help the queue rather than blocking.
static constexpr int32_t nReservedProcessors = 2;

// Past six workers the extra threads live in the queue list critical section.
static constexpr int32_t nMinWorkers = 1;
static constexpr int32_t nMaxWorkers = 6;

// Frames of GPU work the CPU may queue ahead, and the engine's own ceiling: above five fences per
// GPU it rebuilds the ring's D3D queries every frame.
static constexpr int32_t nFrameQueueDepth = 3;
static constexpr int32_t nFrameQueueCeilingPerGpu = 5;

// GPU count comes from a virtual call, so bound it before multiplying.
static constexpr int32_t nMaximumGpus = 8;

// Worker back off in iterations, not time: the loop body is a critical section round trip.
static constexpr uint32_t nPauseIterations = 64;
static constexpr uint32_t nPausesPerIteration = 8;

// Visits further apart than this are separate episodes; the ramp restarts.
static constexpr int64_t nEpisodeGapMicroseconds = 1000;

// Renderer object, as the frame limiter sees it in ESI.
static constexpr uint32_t nRendererSettings = 0x2C;
static constexpr uint32_t nRendererFrameCounter = 0x338;

// CRenderSettings gfx_MaxFps, an int. The limiter clamps below 1 and skips itself at 1000 up.
static constexpr uint32_t nSettingsMaxFps = 0xC8;
static constexpr int32_t nLimiterDisabledFps = 1000;

// Left for the loop to spin, one per timer accuracy; the third is a stock 15.6ms tick plus room.
static constexpr int64_t nHighResolutionMarginMicroseconds = 400;
static constexpr int64_t nPlainMarginMicroseconds = 2000;
static constexpr int64_t nCoarseMarginMicroseconds = 20000;

// A deadline further out than this is spun, not slept.
static constexpr int64_t nMaximumWaitMicroseconds = 200000;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Written by the ini watch thread, read by the render thread and every job worker.
static std::atomic<bool> bUtilisation = true;

// The quality table reads JOB_THREADS too, so the pool's call is told apart by return address.
static uintptr_t nJobPoolReturnAddress = 0;

// Thresholds in counter ticks: hot paths compare rather than divide.
static int64_t nCounterFrequency = 0;
static int64_t nEpisodeGapTicks = 0;
static int64_t nMinimumWaitTicks = 0;
static int64_t nMaximumWaitTicks = 0;

static SafetyHookInline ThreadCountHook{};
static SafetyHookMid WorkerStarvedHook{};
static SafetyHookMid FrameQueueHook{};
static SafetyHookMid FrameLimiterHook{};

static int64_t Counter()
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static int64_t ToTicks(int64_t nMicroseconds)
{
    return (nCounterFrequency * nMicroseconds) / 1000000;
}

// Input bounded by nMaximumWaitTicks, so the multiply cannot overflow.
static int64_t ToMicroseconds(int64_t nTicks)
{
    return (nTicks * 1000000) / nCounterFrequency;
}

// Patterns wildcard their immediates, so check a lifted pointer before dereferencing it.
static bool WithinDunia(const void* pAddress)
{
    if (pAddress == nullptr || hDunia == nullptr)
        return false;

    const auto* pBase = reinterpret_cast<const uint8_t*>(hDunia);
    const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase);
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(pBase + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const auto* pByte = reinterpret_cast<const uint8_t*>(pAddress);
    return pByte >= pBase && pByte < pBase + pNt->OptionalHeader.SizeOfImage;
}

// Affinity mask, not GetSystemInfo, so CpuAffinity narrows the pool too.
static int32_t WorkerThreadCount()
{
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    int32_t nProcessors = 0;

    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
    {
        for (DWORD_PTR bit = 1; bit != 0; bit <<= 1)
        {
            if (processMask & bit)
                ++nProcessors;
        }
    }

    if (nProcessors <= 0)
    {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        nProcessors = static_cast<int32_t>(info.dwNumberOfProcessors);
    }

    return std::clamp(nProcessors - nReservedProcessors, nMinWorkers, nMaxWorkers);
}

// CThreadingConfig::GetThreadCount. __thiscall, one stack argument, callee cleanup of 4, so
// fastcall with EDX carried through matches.
static int32_t __fastcall ThreadCount(void* pThis, void* pEdx, const char* pszName)
{
    const auto nReturn = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto nStock = ThreadCountHook.fastcall<int32_t>(pThis, pEdx, pszName);

    if (!bUtilisation || nReturn != nJobPoolReturnAddress)
        return nStock;

    // Never below the config, so OverrideThreadingConfig.xml still wins. Not std::max: no NOMINMAX
    // here, and Windows.h's max macro eats it.
    const auto nWanted = WorkerThreadCount();
    return nWanted > nStock ? nWanted : nStock;
}

// Worker dequeue retry, on the turn the pop came back empty. State is per thread. Context is not
// written, so AL stays zero for the CMP [EDX+0x14],AL the loop resumes on.
static void WorkerStarved(SafetyHookContext&)
{
    static thread_local uint32_t nIterations = 0;
    static thread_local int64_t nPreviousVisit = 0;

    if (!bUtilisation || nCounterFrequency <= 0)
        return;

    if (Counter() - nPreviousVisit > nEpisodeGapTicks)
        nIterations = 0;

    if (++nIterations < nPauseIterations)
    {
        for (uint32_t i = 0; i < nPausesPerIteration; ++i)
            YieldProcessor();
    }
    else
    {
        // SwitchToThread, not Sleep(0): it considers threads below this one's priority too.
        SwitchToThread();
    }

    // Stamped last, so the next gap measures the loop body and not the yield above.
    nPreviousVisit = Counter();
}

// At the ring size comparison, EAX the GPU count. Zero is the D3D10 path, fences off. The ring is
// sized one fence per GPU, so a single GPU gets one frame of run ahead.
static void FrameQueueDepth(SafetyHookContext& regs)
{
    if (!bUtilisation)
        return;

    const auto nGpus = static_cast<int32_t>(regs.eax);
    if (nGpus <= 0 || nGpus > nMaximumGpus)
        return;

    // Never shallower than asked for, never deeper than the engine will build.
    const auto nDepth = std::clamp(nFrameQueueDepth, nGpus, nGpus * nFrameQueueCeilingPerGpu);
    regs.eax = static_cast<uintptr_t>(nDepth);
}

// One timer per limiter thread. INVALID_HANDLE_VALUE = not tried, null = unavailable, so the
// create runs once. Never closed: the thread outlives the module.
static HANDLE WaitableTimer(int64_t& nMargin)
{
    static thread_local HANDLE hTimer = INVALID_HANDLE_VALUE;
    static thread_local int64_t nTimerMargin = nCoarseMarginMicroseconds;

    if (hTimer == INVALID_HANDLE_VALUE)
    {
        hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        nTimerMargin = nHighResolutionMarginMicroseconds;

        // Flag rejected before Windows 10 1803; a plain timer rounds to the global timer period.
        if (hTimer == nullptr)
        {
            hTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            nTimerMargin = nPlainMarginMicroseconds;
        }

        if (hTimer == nullptr)
            nTimerMargin = nCoarseMarginMicroseconds;
    }

    nMargin = nTimerMargin;
    return hTimer;
}

// Sleeps out all but the margin, leaving the loop to land on the deadline.
static void Wait(int64_t nRemaining)
{
    int64_t nMargin = nCoarseMarginMicroseconds;
    auto hTimer = WaitableTimer(nMargin);

    const auto nWait = nRemaining - nMargin;
    if (nWait <= 0)
        return;

    if (hTimer != nullptr)
    {
        LARGE_INTEGER due{};
        due.QuadPart = -(nWait * 10); // Relative, in 100ns units.

        if (SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(hTimer, INFINITE);
            return;
        }
    }

    Sleep(static_cast<DWORD>(nWait / 1000));
}

// The limiter busy waits on the render thread with gfx_MaxFps set. Hooked in its spin, just before
// its QueryPerformanceCounter; ESI is the renderer, not reloaded across the loop.
static void FrameLimiterWait(SafetyHookContext& regs)
{
    if (!bUtilisation || nCounterFrequency <= 0)
        return;

    const auto* pRenderer = reinterpret_cast<const uint8_t*>(regs.esi);
    if (pRenderer == nullptr)
        return;

    const auto* pSettings = *reinterpret_cast<const uint8_t* const*>(pRenderer + nRendererSettings);
    if (pSettings == nullptr)
        return;

    const auto nMaxFps = *reinterpret_cast<const int32_t*>(pSettings + nSettingsMaxFps);
    if (nMaxFps < 1 || nMaxFps >= nLimiterDisabledFps)
        return;

    // Written after the loop, so during it this is the previous frame's release point.
    const auto nPreviousFrame = *reinterpret_cast<const int64_t*>(pRenderer + nRendererFrameCounter);
    if (nPreviousFrame <= 0)
        return;

    const auto nRemaining = (nPreviousFrame + nCounterFrequency / nMaxFps) - Counter();

    if (nRemaining <= nMinimumWaitTicks || nRemaining > nMaximumWaitTicks)
        return;

    Wait(ToMicroseconds(nRemaining));
}

class Utilisation
{
public:
    Utilisation()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Depth, back off and limiter follow the ini live; worker count is read once, at pool
            // construction.
            BindBool(bUtilisation, PREF_UTILISATION);

            // Zero disables the two clock paced hooks and leaves the others working.
            LARGE_INTEGER frequency{};
            if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            {
                nCounterFrequency = frequency.QuadPart;
                nEpisodeGapTicks = ToTicks(nEpisodeGapMicroseconds);
                nMinimumWaitTicks = ToTicks(nHighResolutionMarginMicroseconds);
                nMaximumWaitTicks = ToTicks(nMaximumWaitMicroseconds);
            }

            // CJobScheduler asking for JOB_THREADS. Match+0x10 is the instruction after the call.
            if (auto* pSite = static_cast<uint8_t*>(dunia_find("8B 0D ? ? ? ? 68 ? ? ? ? E8 ? ? ? ? 8B F0 3B F7 89 74 24 10 7E")))
            {
                // Pushed string is wildcarded, so confirm the site by what it names.
                const auto* pszName = *reinterpret_cast<const char* const*>(pSite + 7);
                if (WithinDunia(pszName) && strcmp(pszName, "JOB_THREADS") == 0)
                    nJobPoolReturnAddress = reinterpret_cast<uintptr_t>(pSite + 0x10);
            }

            // CThreadingConfig::GetThreadCount, entry through the name's std::string.
            if (auto* p = dunia_find("83 EC 24 53 56 57 8D 44 24 0F 8B F9 50 8D 4C 24 1C 33 DB 51 C7 44 24 1C ? ? ? ? C7 44 24 34 0F 00 00 00 88 5C 24 17"); p && nJobPoolReturnAddress != 0)
                ThreadCountHook = safetyhook::create_inline(p, ThreadCount);

            // Dequeue retry loop. Match+0x0D is the shutdown flag re-read, the only instruction on
            // the empty handed path.
            if (auto* p = dunia_find("8B CE E8 ? ? ? ? 8B F8 85 FF 75 0D 8B 96 E0 00 00 00 38 42 14 74 E8", 0x0D))
                WorkerStarvedHook = safetyhook::create_mid(p, WorkerStarved);

            // Fence ring size check; the match spans both paths reaching the comparison.
            if (auto* p = dunia_find("FF D0 EB 02 33 C0 39 46 34 74 0A 8B CE 89 46 34 E8 ? ? ? ? E8 ? ? ? ? 8B 4E 34 85 C9", 6))
                FrameQueueHook = safetyhook::create_mid(p, FrameQueueDepth);

            // Limiter busy wait. Match+6 is the counter call.
            if (auto* p = dunia_find("DF F1 DD D8 76 ? E8 ? ? ? ? 8B C8 8B F9 2B BE 38 03 00 00 8B C2 1B 86 3C 03 00 00", 6))
                FrameLimiterHook = safetyhook::create_mid(p, FrameLimiterWait);
        };
    }
} Utilisation;
