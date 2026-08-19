/*
  Framerate dependent movement bugs inherited from the 30fps console target. Each is a distance
  divided by, or measured in, the frame time.

  Jump height collapses at high framerates

  CPawnData+0x4B0 is the live jump height, seeded from the entity library's fJumpHeight at spawn,
  which is why that stat needs a new game to take. Each frame FUN_100847C0 packs it, the desired
  velocity and a jumped-this-frame bit out of CPawnData+0x2D4 into a parameter block, and
  FUN_104AF2E0 writes them to m_wantJump at controller+0x6C and m_jumpHeight at controller+0x5C.
  hkpCharacterStateOnGround::change (0x10BE4BA0) sees m_wantJump and enters the jumping state, whose
  update (0x10BE5430) launches at sqrt(2 * |gravity| * m_jumpHeight): 6.0 m/s at height 1 and
  gravity 18, with no frame time in it.

  The loss is in the three frames after launch. FUN_104B0A20 casts downwards each frame and sets
  m_isSupported (input+0x31) for anything within ~3cm. The first airborne frame is 17.6cm up at
  30fps but only 0.86cm at 730fps, so hkpCharacterStateInAir::change (0x10BE5100) still sees
  support and lands a character rising at 6 m/s. The on-ground state then cancels vertical
  velocity, bounded only by hkpCharacterContext's m_maxLinearAcceleration (625) * dt clamp. Apex
  0.99m at 30fps, 0.38m at 730fps.

  The fix clears m_isSupported across the one change() call while the character is rising faster
  than fRisingVelocity, then puts it back. The velocity is measured relative to the surface below,
  so a character riding a lift upwards still counts as landed. Clearing around the call instead of
  skipping it leaves the ladder transition on input+0x30 working. No time limit is needed: gravity
  is integrated every frame whatever the state.

  Characters bounce uncontrollably at high framerates

  -norigidchars cures this by clearing one byte at [0x11613D60]+0x25, read only by FUN_10493CA0,
  which selects between two whole controllers:

        flag set (default)          alloc 0x100, ctor FUN_104B0F40, update FUN_104B1720
                                    -> hkpCharacterRigidBody, a dynamic body that exchanges
                                       impulses with the physics world
        flag clear (-norigidchars)  alloc 0x130, ctor FUN_104AE870, update FUN_104B0D20
                                    -> hkpCharacterProxy, swept through the world, never reacts

  Demoting every character to a kinematic proxy is why it works and why it is not the fix.

  hkpCharacterRigidBody::checkSupport (0x10BE9F40) probes downwards at exactly 1 m/s over the frame
  delta, so the probe reach in metres is numerically the frame time: 33.3mm at 30fps, 2.58mm at
  360fps. A rigid body floats up to 5.8mm above its resting height inside Havok's contact
  tolerance, so support drops out while the character is standing still. Gravity is then applied,
  the solver ejects the sunk body (+8.68 m/s on one observed frame), and 625 * dt cannot cancel
  that within the three FRAME support hysteresis at controller+0xB4.

  Flooring the step info delta time (stepInfo+0x08) to 1/30 for the duration of the call gives the
  probe its 30fps reach back. checkSupport reads the value only through the simplex input's own
  copy; FUN_104B10D0 writes the frame time and calls checkSupport before anything else reads it,
  and the state machine's own integration does not run until FUN_104B1720 continues afterwards.
  Rigid body only, since the proxy has its own support test at 0x10BE7F80.

  Rejected: scaling m_maxLinearAcceleration (hkpCharacterContext::update, 0x10BE46D0) by (1/30)/dt.
  Implemented and measured, no effect. 625 * dt is already a rate; the per FRAME budget would grant
  6250 m/s^2 at 300fps and loosen ordinary walking acceleration too. The hysteresis counter at
  controller+0xB4 is left unpatched because nothing provokes it once the root motion fix is in.

  NPCs bounce uncontrollably at high framerates

  FUN_100ACD10 turns animation root motion into a desired velocity as |displacement| / dt, clamped
  to 20. Root motion arrives on the animation's schedule and stays 30Hz sized as dt shrinks:
  0.024m per frame at 30fps against >0.060m at 332fps, so the desired speed pins at the clamp and
  the rigid body is driven at 20 m/s while going nowhere. Flooring the divisor at 1/30 gives the
  speed 30fps would compute; at or below 30fps nothing changes.

  A GUI sound is started once a frame and starves the rest of the mix

  Dunia+80BB1F is a call through the sound system's play slot at vtable +9Ch, in the diamond
  counter's update, once per update whatever an update is worth. Measured at Dare's own play,
  Dunia+A3D9C0, which everything the game plays funnels through, one scene at two rates:

      30fps    starts 33.3ms apart,  16 starts, mean voice life 1.377s, 25 voices live
      240fps   starts  4.2ms apart, 114 starts, mean voice life 0.889s, 76 voices live

  The mean life falling with the rate is the damage: the pool fills with copies of one sound and
  Dare takes voices back from whatever else is playing, and a streamed one loses.

  The call is guarded already: the id is fetched by name just above, and when the fetch answers the
  not-found value at Dunia+E82B14 the branch at Dunia+80BB03 skips the play. A start arriving less
  than 1/30s after the last one for the same id is handed that value, so the engine takes its own
  skip. Measured after: 15 starts 33.4ms apart, mean life 1.342s, 28 voices live. At or below 30fps
  none is turned away.

  The diamond counter rolls up at framerate

  The same element, Dunia+80B7A0, owns the number as well as that sound. Its fields:

      +0x88   the CEconomyComponent, whose +0x10 is the wallet
      +0x2C8  the count the element last settled on
      +0x2CC  the number on screen, which is what rolls
      +0x2C0  single steps left to roll
      +0x2B8  a timer, set to 5.0 when a roll starts and reduced by the frame time

  A change sets +2C0 to the difference and +2B8 to the timer, and then every update, while +2C0 is
  not zero, +2CC moves by one and +2C0 comes down by one. One digit per update, so the roll is as
  long as a frame is short. Fifteen diamonds:

      30fps    steps 33.3ms apart, roll 0.467s
      240fps   steps  4.2ms apart, roll 0.062s

  The timer only ends a roll early, so it does not hold the rate.

  The fix withholds a step until 1/30s has passed since that element's last one. The element already
  skips a step when +2C0 reads zero, at Dunia+80B904 counting up and Dunia+80B9E9 counting down, so a
  withheld step is made by handing that test a zero. The frame time is taken off +2B8 between the
  test and the branch either way, so a roll long enough to reach the timer still ends there.
  Measured after at 240fps: steps 33.4ms apart, roll 0.468s, against 0.467s at 30fps.

  None of the five has an ini key. They are bugs rather than preferences.
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
