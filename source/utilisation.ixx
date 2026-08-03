/*
  Four places where Dunia leaves the machine idle, all inherited from a fixed three core console
  target with a known GPU.

  Job pool size

  CThreadingConfig parses engine\settings\DefaultThreadingConfig.xml and merges
  engine\settings\OverrideThreadingConfig.xml over it into CThreadConfigInfo records.
  GetThreadCount (FUN_102b3e80) answers a group name from one record as
  min(MaxThreadCnt, max(MinThreadCnt, ThreadCnt)), or min(MaxThreadCnt, max(MinThreadCnt,
  cores - ThreadCnt)) when RelativeToCoreCnt is set, and 0 when there is no record. The
  CJobScheduler constructor (FUN_102b2ea0) creates exactly that many workers, unclamped.

  The shipped MaxThreadCnt exists only in the data files, but FUN_104b8c90 re-reads JOB_THREADS,
  clamps it into 1 to 3 and switches between three quality tables on the result, so 3 is the
  ceiling the engine was written against. The hook goes on GetThreadCount and compares its return
  address against the one call site inside the CJobScheduler constructor, leaving RENDER_THREAD,
  PHYSIC_THREADS and FUN_104b8c90's own read on the stock answer. Raising FUN_104b8c90's input
  would move it to a different quality table, which is a visual change.

  Worker starvation

  Workers block on a counting semaphore. What follows the wake does not:

        102b2630   MOV  ECX,ESI
        102b2632   CALL 102b2540          try to pop a job
        102b2637   MOV  EDI,EAX
        102b2639   TEST EDI,EDI
        102b263b   JNZ  102b264a          got one
        102b263d   MOV  EDX,[ESI+0xE0]    re-read the shutdown flag
        102b2643   CMP  byte ptr [EDX+0x14],AL
        102b2646   JZ   102b2630

  FUN_102b2540 takes the worker's queue list critical section and a TryEnterCriticalSection per
  queue on every turn, so a worker that wakes to an empty queue saturates a core and contends for
  the lock the productive workers need. Three causes: FUN_102b27a0 releases the semaphore for a
  whole batch before pushing any of it, FUN_102b1f50 withholds a job whose barrier sequence has not
  been reached and also returns empty on a failed TryEnterCriticalSection, and FUN_102b2330 takes
  tokens off the same semaphore with a zero timeout and hands them back. The wasted core count
  scales with the pool, so the back off matters more once the pool is sized to the machine.

  GPU frame queue

  FUN_1037d390 waits each frame on a D3DQUERYTYPE_EVENT query issued a fixed number of frames ago,
  polling GetData with D3DGETDATA_FLUSH. FUN_1037d120 sizes the ring from the GPU count, not from
  any latency setting:

        1037d3cd   CALL EAX               backend vtable +0x12C, returns backend+0x08
        1037d3d3   CMP  [ESI+0x34],EAX    ring size still matches?
        1037d3d6   JZ   1037d3e2
        1037d3da   MOV  [ESI+0x34],EAX
        1037d3dd   CALL 1037d120          rebuild the ring

  backend+0x08 is written once in the D3D9 device init as max(initParams+0x24, 1) and the debug
  overlay prints it as "Multi-GPUs : %d". One fence per GPU is an alternate frame rendering
  construct, so a single GPU gets one frame of CPU run ahead. FUN_103430a0 arms the mechanism only
  when that count is 1, which is every ordinary machine. FUN_1037d120 caps what it builds at five
  times the GPU count and stores what it built, so a request above that ceiling fails the size
  check every frame and rebuilds the ring's D3D queries every frame.

  Frame limiter

  With MaxFrameRate set, the limiter at the end of FUN_10345c70 runs on the render thread with no
  yield of any kind:

        10346436   JBE  1034649A          already slow enough
        10346438   CALL 10C5DB40          QueryPerformanceCounter
        ...        compute 1/dt and compare against gfx_MaxFps
        10346494   JA   10346438

  The hook reads the deadline the loop is spinning towards, renderer+0x338 plus one period of
  gfx_MaxFps at renderer+0x2C's +0xC8, and waits out all but the last fraction of a millisecond on
  a waitable timer. The loop's own comparison is untouched, so it spins the remainder as before and
  the pacing is unchanged. The timer is created with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION so this
  does not depend on HighPrecisionTimer having raised the global period.

  Measured and rejected

  Breaking the starvation loop back out to the semaphore, by writing a non-zero AL so its
  CMP byte ptr [EDX+0x14],AL stops comparing equal, hangs the game within seconds. The loop has no
  other exit, so a worker that gives up spends its token on nothing and a job made available by a
  lock that was merely busy is left with nobody holding a token for it. PAUSE and SwitchToThread
  are the ceiling.

  MaxDriverBufferedFrames is not the frame queue knob. It has no reader in Dunia.dll outside the
  debug overlay.

  StreamingThread runs at THREAD_PRIORITY_HIGHEST against NORMAL for everything that draws
  (FUN_1023bc50, priority store at 0x1023bd1b). Dropping it to NORMAL measured no improvement.
  Folding the two job system locks built with spin count 0 in with the thirty nine built with
  0x1000, by turning FUN_1000e120's CMP EDX,-1 / JNZ into CMP EDX,1 / JGE, measured no improvement
  either.
*/

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

// Held back for the main thread and the render thread, both of which help the job queue while they
// wait rather than blocking.
static constexpr int32_t nReservedProcessors = 2;

// Past six workers the extra threads spend their time in the queue list critical section.
static constexpr int32_t nMinWorkers = 1;
static constexpr int32_t nMaxWorkers = 6;

// Frames of GPU work the CPU may queue ahead, and FUN_1037d120's own ceiling on it.
static constexpr int32_t nFrameQueueDepth = 3;
static constexpr int32_t nFrameQueueCeilingPerGpu = 5;

// The GPU count arrives from a virtual call, so it is bounded before being multiplied.
static constexpr int32_t nMaximumGpus = 8;

// Worker back off. Iterations, not time: the loop body is a critical section round trip, so the
// PAUSE stage is a few microseconds.
static constexpr uint32_t nPauseIterations = 64;
static constexpr uint32_t nPausesPerIteration = 8;

// Visits further apart than this are separate starvation episodes, so the ramp restarts. Measured
// across the game's loop body alone, since the timestamp is taken after this hook has yielded.
static constexpr int64_t nEpisodeGapMicroseconds = 1000;

// Renderer object, as the frame limiter sees it in ESI.
static constexpr uint32_t nRendererSettings = 0x2C;
static constexpr uint32_t nRendererFrameCounter = 0x338;

// CRenderSettings, gfx_MaxFps. An int: the limiter clamps it with CMP EAX,1 at 0x1034640c and skips
// itself at or above 1000 with CMP EAX,0x3e8 at 0x103463e3.
static constexpr uint32_t nSettingsMaxFps = 0xC8;
static constexpr int32_t nLimiterDisabledFps = 1000;

// Left for the loop to spin, one value per timer accuracy. The third is a stock 15.6ms tick plus
// room, so a wait shorter than that is skipped rather than risked.
static constexpr int64_t nHighResolutionMarginMicroseconds = 400;
static constexpr int64_t nPlainMarginMicroseconds = 2000;
static constexpr int64_t nCoarseMarginMicroseconds = 20000;

// A deadline reading further out than this is spun out rather than slept through.
static constexpr int64_t nMaximumWaitMicroseconds = 200000;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Written from the ini file watch thread, read from the render thread and every job worker.
static std::atomic<bool> bUtilisation = true;

static uintptr_t nJobPoolReturnAddress = 0;

// Thresholds in counter ticks, so the hot paths compare rather than divide and a stale zero
// timestamp cannot overflow a microsecond conversion.
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

// Only handed a value already bounded by nMaximumWaitTicks, so the multiply cannot overflow.
static int64_t ToMicroseconds(int64_t nTicks)
{
    return (nTicks * 1000000) / nCounterFrequency;
}

// The patterns wildcard their immediate operands, so a pointer lifted from one is confirmed to land
// inside Dunia before anything dereferences it.
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

// From the process affinity mask rather than GetSystemInfo, so CpuAffinity narrows the pool too.
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

// CThreadingConfig::GetThreadCount. __thiscall with one stack argument and a callee cleanup of four
// bytes, so the trampoline is entered fastcall with EDX carried through.
static int32_t __fastcall ThreadCount(void* pThis, void* pEdx, const char* pszName)
{
    const auto nReturn = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto nStock = ThreadCountHook.fastcall<int32_t>(pThis, pEdx, pszName);

    if (!bUtilisation || nReturn != nJobPoolReturnAddress)
        return nStock;

    // Never below the config, so a hand written OverrideThreadingConfig.xml still wins. Written out
    // rather than std::max, which Windows.h's max macro eats: NOMINMAX is not defined here.
    const auto nWanted = WorkerThreadCount();
    return nWanted > nStock ? nWanted : nStock;
}

// The worker's dequeue retry, on the turn where the pop came back empty. State is per thread: every
// worker runs its own copy of the loop. Nothing writes the context, so AL stays zero for the
// CMP byte ptr [EDX+0x14],AL the loop resumes on.
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
        // SwitchToThread over Sleep(0): it considers every runnable thread on the processor, where
        // Sleep(0) considers only those at the same priority.
        SwitchToThread();
    }

    // Stamped last, so the next visit's gap is the loop body rather than the loop body plus however
    // long the yield above kept this thread off the processor.
    nPreviousVisit = Counter();
}

// At the ring size comparison, EAX holding the GPU count the ring would otherwise be sized to. Zero
// is the D3D10 path, where the fence system is off.
static void FrameQueueDepth(SafetyHookContext& regs)
{
    if (!bUtilisation)
        return;

    const auto nGpus = static_cast<int32_t>(regs.eax);
    if (nGpus <= 0 || nGpus > nMaximumGpus)
        return;

    // Never shallower than the ring the engine asked for, never deeper than the one it will build.
    const auto nDepth = std::clamp(nFrameQueueDepth, nGpus, nGpus * nFrameQueueCeilingPerGpu);
    regs.eax = static_cast<uintptr_t>(nDepth);
}

// One timer per thread that runs the limiter, in practice the render thread alone.
// INVALID_HANDLE_VALUE means not yet tried and null means tried and unavailable, so the create runs
// once. Never closed: the thread outlives the module.
static HANDLE WaitableTimer(int64_t& nMargin)
{
    static thread_local HANDLE hTimer = INVALID_HANDLE_VALUE;
    static thread_local int64_t nTimerMargin = nCoarseMarginMicroseconds;

    if (hTimer == INVALID_HANDLE_VALUE)
    {
        hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        nTimerMargin = nHighResolutionMarginMicroseconds;

        // Before Windows 10 1803 the flag is rejected. A plain timer still rounds to the global
        // timer period, which HighPrecisionTimer has usually already taken to 1ms.
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

// Given the whole remaining wait, gives back all but the margin the loop needs to land on the
// deadline itself.
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

// Inside the limiter's spin, immediately before its QueryPerformanceCounter call. ESI is the
// renderer, written once at 0x10345c83 and never reloaded across the loop.
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

    // Written at 0x103464a8, after the loop, so during it this holds the point the previous frame
    // was released, which is what the loop measures against.
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
            bUtilisation = JackalFixSettings.GetInt(PREF_UTILISATION) != 0;

            // The two hooks that pace against the clock check this and do nothing while it is zero,
            // so a counter that cannot be read costs those fixes and not the others.
            LARGE_INTEGER frequency{};
            if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            {
                nCounterFrequency = frequency.QuadPart;
                nEpisodeGapTicks = ToTicks(nEpisodeGapMicroseconds);
                nMinimumWaitTicks = ToTicks(nHighResolutionMarginMicroseconds);
                nMaximumWaitTicks = ToTicks(nMaximumWaitMicroseconds);
            }

            // The CJobScheduler constructor asking for JOB_THREADS, matched from the singleton load
            // through the store of the result. Match+0x10 is the instruction after the call.
            auto poolPattern = dunia_pattern("8B 0D ? ? ? ? 68 ? ? ? ? E8 ? ? ? ? 8B F0 3B F7 89 74 24 10 7E");
            if (!poolPattern.empty())
            {
                auto* pSite = poolPattern.get_first<uint8_t>();

                // The pushed string is wildcarded, so the site is confirmed by what it names.
                const auto* pszName = *reinterpret_cast<const char* const*>(pSite + 7);
                if (WithinDunia(pszName) && strcmp(pszName, "JOB_THREADS") == 0)
                    nJobPoolReturnAddress = reinterpret_cast<uintptr_t>(pSite + 0x10);
            }

            // CThreadingConfig::GetThreadCount, matched from entry through the std::string the name
            // is copied into. The absolute address in the prologue is wildcarded.
            auto threadCountPattern = dunia_pattern("83 EC 24 53 56 57 8D 44 24 0F 8B F9 50 8D 4C 24 1C 33 DB 51 C7 44 24 1C ? ? ? ? C7 44 24 34 0F 00 00 00 88 5C 24 17");
            if (!threadCountPattern.empty() && nJobPoolReturnAddress != 0)
                ThreadCountHook = safetyhook::create_inline(threadCountPattern.get_first(), ThreadCount);

            // The dequeue retry loop, matched from the pop call through the branch back. Match+0x0D
            // is the shutdown flag re-read, the only instruction on the empty handed path.
            auto starvePattern = dunia_pattern("8B CE E8 ? ? ? ? 8B F8 85 FF 75 0D 8B 96 E0 00 00 00 38 42 14 74 E8");
            if (!starvePattern.empty())
                WorkerStarvedHook = safetyhook::create_mid(starvePattern.get_first(0x0D), WorkerStarved);

            // The fence ring size check. Matched from the GPU count call through the rebuild, so
            // both paths that reach the comparison are inside the match.
            auto frameQueuePattern = dunia_pattern("FF D0 EB 02 33 C0 39 46 34 74 0A 8B CE 89 46 34 E8 ? ? ? ? E8 ? ? ? ? 8B 4E 34 85 C9");
            if (!frameQueuePattern.empty())
                FrameQueueHook = safetyhook::create_mid(frameQueuePattern.get_first(6), FrameQueueDepth);

            // The limiter's busy wait, matched from the comparison that enters it through the
            // subtraction of the previous frame's counter. Match+6 is the counter call.
            auto limiterPattern = dunia_pattern("DF F1 DD D8 76 ? E8 ? ? ? ? 8B C8 8B F9 2B BE 38 03 00 00 8B C2 1B 86 3C 03 00 00");
            if (!limiterPattern.empty())
                FrameLimiterHook = safetyhook::create_mid(limiterPattern.get_first(6), FrameLimiterWait);

            // The fence depth, the back off and the limiter are read where they are used, so they
            // follow the ini live. The worker count is read once, when the pool is built.
            JackalFix::onIniFileChange() += []()
            {
                bUtilisation = JackalFixSettings.GetInt(PREF_UTILISATION) != 0;
            };
        };
    }
} Utilisation;
