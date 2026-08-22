/*
  Framerate dependent movement bugs inherited from the 30fps console target.
*/

module;

#include <common.hxx>

export module fpsfixes;

import common;
import dunia;

// hkpCharacterInput.
static constexpr uint32_t nInputIsSupported = 0x31;
static constexpr uint32_t nInputUp = 0x10;
static constexpr uint32_t nInputSurfaceVelocity = 0x50;
static constexpr uint32_t nInputVelocity = 0x90;

// Upward m/s relative to the surface below at which the character counts as rising. Sits between
// two well separated populations: a character at rest carries about 0.02 m/s of residual from the
// controller pushing itself out of penetration, a jump launches at 6 m/s, and every real landing
// measured arrives strongly negative.
static constexpr float fRisingVelocity = 0.5f;

// hkpStepInfo, as hkpCharacterRigidBody::checkSupport is handed it.
static constexpr uint32_t nStepInfoDeltaTime = 0x08;

// The frame time the movement constants were tuned against.
static constexpr float fReferenceDelta = 1.0f / 30.0f;

// Floor for the support probe's delta time. The probe travels 1 m/s, so this is also its reach:
// 33.3mm.
static constexpr float fSupportProbeMinDelta = fReferenceDelta;

// Delta time argument to FUN_100ACD10 relative to ESP at its first instruction: return address,
// owner pointer, frame time.
static constexpr uint32_t nRootMotionDeltaSlot = 0x08;

// Into the counter's two step patterns: the TEST of the steps-left field, not the load ahead of it.
// A mid hook runs before the instruction it is placed on and that instruction is then executed from
// the trampoline, so a hook on the load would have its own write overwritten by the load itself.
static constexpr ptrdiff_t nCounterStepTest = 0x0E;

// Into the GUI sound's pattern: the CMP that decides whether the id is playable, and the absolute
// address of the not-found value it is compared against.
static constexpr ptrdiff_t nGuiSoundCompare = 0x0B;
static constexpr ptrdiff_t nGuiSoundNotFound = 0x0D;

static SafetyHookInline InAirChangeHook{};
static SafetyHookInline CheckSupportHook{};
static SafetyHookMid RootMotionSpeedHook{};
static SafetyHookMid GuiSoundHook{};
static SafetyHookMid CounterStepUpHook{};
static SafetyHookMid CounterStepDownHook{};

// The id the fetch answers when there is no sound, read out of the compare rather than assumed.
static const int32_t* pGuiSoundNotFound = nullptr;

// Longest a key is remembered after it last came up. Only bounds the tables.
static constexpr double fThrottleForget = 5.0;

// True when this key has not come up inside a frame of the reference rate, and takes the slot if so.
// The GUI update is the engine's own thread, but the tables are behind a lock rather than on the
// assumption that they always will be.
static std::mutex ThrottleMutex;

static bool ThrottleDue(std::map<uintptr_t, int64_t>& Table, uintptr_t nKey)
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);

    if (frequency.QuadPart == 0)
        return true;

    const auto nNow = counter.QuadPart;
    const auto nInterval = static_cast<int64_t>(static_cast<double>(frequency.QuadPart) * fReferenceDelta);

    std::scoped_lock lock(ThrottleMutex);

    auto entry = Table.find(nKey);
    if (entry != Table.end() && nNow - entry->second < nInterval)
        return false;

    if (entry != Table.end())
        entry->second = nNow;
    else
        Table.emplace(nKey, nNow);

    const auto nForget = static_cast<int64_t>(static_cast<double>(frequency.QuadPart) * fThrottleForget);
    for (auto it = Table.begin(); it != Table.end();)
        it = (nNow - it->second > nForget) ? Table.erase(it) : std::next(it);

    return true;
}

// Keyed on the sound id, since the element plays one sound at a time and two elements asking for the
// same sound in the same frame is the case being cut down.
static std::map<uintptr_t, int64_t> GuiSoundLast;

// Keyed on the element, so two counters rolling at once each keep their own rate.
static std::map<uintptr_t, int64_t> CounterStepLast;

// Dunia+80BAFD, with ESI holding the id the name lookup just answered and the branch that skips the
// play immediately after. Nothing float is live here and the hook lands on a compare.
static void GuiSound(SafetyHookContext& regs)
{
    if (pGuiSoundNotFound == nullptr)
        return;

    const auto nSoundId = static_cast<int32_t>(regs.esi);

    // Already on its way to the skip. Nothing to hold an interval against.
    if (nSoundId == *pGuiSoundNotFound)
        return;

    if (!ThrottleDue(GuiSoundLast, static_cast<uintptr_t>(static_cast<uint32_t>(nSoundId))))
        regs.esi = static_cast<uintptr_t>(*pGuiSoundNotFound);
}

// Dunia+80B904 and Dunia+80B9E9, the TEST of the steps-left field in each of the counter's two
// directions. The branch it feeds skips the step when it reads zero, so a zero here is a step
// withheld. ESI is the element. The frame time is taken off the roll's timer between this and the
// branch either way, so withholding a step does not stop the timer.
static void CounterStep(SafetyHookContext& regs)
{
    if (regs.eax == 0)
        return;

    if (!ThrottleDue(CounterStepLast, static_cast<uintptr_t>(regs.esi)))
        regs.eax = 0;
}

static float UpwardVelocity(uintptr_t nInput)
{
    const auto* pUp = reinterpret_cast<const float*>(nInput + nInputUp);
    const auto* pVelocity = reinterpret_cast<const float*>(nInput + nInputVelocity);
    const auto* pSurface = reinterpret_cast<const float*>(nInput + nInputSurfaceVelocity);

    return pUp[0] * (pVelocity[0] - pSurface[0])
        + pUp[1] * (pVelocity[1] - pSurface[1])
        + pUp[2] * (pVelocity[2] - pSurface[2]);
}

// hkpCharacterStateInAir::change. All three arguments are on the stack and this is unused, but it
// is entered __thiscall, so ECX still has to be carried through the trampoline.
static void __fastcall InAirChange(void* pThis, void* pEdx, void* pContext, void* pInput, void* pOutput)
{
    auto* pSupported = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pInput) + nInputIsSupported);
    const auto nSupported = *pSupported;

    if (nSupported != 0 && UpwardVelocity(reinterpret_cast<uintptr_t>(pInput)) > fRisingVelocity)
        *pSupported = 0;

    InAirChangeHook.fastcall(pThis, pEdx, pContext, pInput, pOutput);

    // Restored unconditionally. The two Far Cry 2 specific character states at controller+0x64 and
    // +0x68 read the flag again later in the same frame.
    *pSupported = nSupported;
}

// hkpCharacterRigidBody::checkSupport. The step info is the character input's own copy at
// input+0x70, so this writes input+0x78, hence the restore before returning.
static void __fastcall CheckSupport(void* pThis, void* pEdx, uint8_t* pStepInfo, void* pSurface)
{
    auto* pDelta = reinterpret_cast<float*>(pStepInfo + nStepInfoDeltaTime);
    const auto fOriginal = *pDelta;

    if (fOriginal < fSupportProbeMinDelta)
        *pDelta = fSupportProbeMinDelta;

    CheckSupportHook.fastcall(pThis, pEdx, pStepInfo, pSurface);

    *pDelta = fOriginal;
}

// FUN_100ACD10 at its first instruction, arguments still where the caller pushed them. A mid hook
// rewriting the stack slot rather than an inline hook with a declared prototype: the slot belongs
// to this call alone, and nothing has to be assumed about the calling convention.
static void RootMotionSpeed(SafetyHookContext& regs)
{
    auto* pDelta = reinterpret_cast<float*>(regs.esp + nRootMotionDeltaSlot);

    if (*pDelta > 0.0f && *pDelta < fReferenceDelta)
        *pDelta = fReferenceDelta;
}

class FpsFixes
{
public:
    FpsFixes()
    {
        JackalFix::onDuniaInitEvent() += []()
            {
                // hkpCharacterStateInAir::change, matched from entry through the m_isSupported test
                // and the setState(HK_CHARACTER_ON_GROUND) it guards. Call displacement wildcarded.
                if (auto* p = dunia_find("8B 44 24 08 80 78 31 00 74 14 8B 4C 24 0C 51 8B 4C 24 08 50 6A 00 E8 ? ? ? ? C2 0C 00 80 78 30 00"))
                    InAirChangeHook = safetyhook::create_inline(p, InAirChange);

                // hkpCharacterRigidBody::checkSupport. The two absolute addresses in its prologue
                // are wildcarded; what makes it unique is the run from the this-pointer save on.
                if (auto* p = dunia_find("55 8B EC 83 E4 F0 81 EC 14 01 00 00 A1 ? ? ? ? 53 8B 1D ? ? ? ? 56 57 8B F1 50 89 74 24 4C FF D3 8B C8 8B 79 04"))
                    CheckSupportHook = safetyhook::create_inline(p, CheckSupport);

                // FUN_100ACD10, the root motion to desired velocity conversion. Matched from entry
                // through the two LEAs that set up the displacement out-parameters.
                if (auto* p = dunia_find("83 EC 1C 56 8B 74 24 24 8D 44 24 04 50 8D 4C 24 0C 51 8B CE E8 ? ? ? ? F3 0F 10 54 24 28"))
                    RootMotionSpeedHook = safetyhook::create_mid(p, RootMotionSpeed);

                // The GUI element's play, matched from the id fetch through the call itself, so the
                // vtable slot and the category are part of what makes it unique. Both call
                // displacements and the not-found address are wildcarded.
                auto guiSoundPattern = dunia_pattern("56 E8 ? ? ? ? 8B F0 83 C4 04 3B 35 ? ? ? ? 74 1C E8 ? ? ? ? D9 EE 8B 10 51 D9 1C 24 6A 00 6A 0C 8B C8 8B 82 9C 00 00 00 56 FF D0 5E 83 C4 20 C2 04 00");
                if (!guiSoundPattern.empty())
                {
                    pGuiSoundNotFound = *guiSoundPattern.get_first<const int32_t*>(nGuiSoundNotFound);
                    GuiSoundHook = safetyhook::create_mid(guiSoundPattern.get_first(nGuiSoundCompare), GuiSound);
                }

                // The counter's two step sites. The bodies are the same either way and part only on
                // the sign of the ADD the step is made with, which is the last byte of each.
                if (auto* p = dunia_find("F3 0F 10 86 B8 02 00 00 8B 86 C0 02 00 00 85 C0 F3 0F 5C 44 24 28 F3 0F 11 86 B8 02 00 00 76 50 83 86 CC 02 00 00 01", nCounterStepTest))
                    CounterStepUpHook = safetyhook::create_mid(p, CounterStep);

                if (auto* p = dunia_find("F3 0F 10 86 B8 02 00 00 8B 86 C0 02 00 00 85 C0 F3 0F 5C 44 24 28 F3 0F 11 86 B8 02 00 00 76 50 83 86 CC 02 00 00 FF", nCounterStepTest))
                    CounterStepDownHook = safetyhook::create_mid(p, CounterStep);
            };
    }
} FpsFixes;
