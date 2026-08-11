/*
  Framerate dependent movement bugs inherited from the 30fps console target. Each is a distance
  divided by, or measured in, the frame time.

  Jump height collapses at high framerates

  How the jump is built: CPawnData+0x4B0 is the live jump height, seeded from the entity library's
  fJumpHeight at spawn, which is why the modding guide says a new game is needed for that stat to
  take. Each frame FUN_100847C0 packs it, the desired velocity and a jumped-this-frame bit out of
  CPawnData+0x2D4 into a parameter block, and FUN_104AF2E0 writes them to m_wantJump at
  controller+0x6C and m_jumpHeight at controller+0x5C. hkpCharacterStateOnGround::change
  (0x10BE4BA0) sees m_wantJump and enters the jumping state, whose update (0x10BE5430) launches at
  sqrt(2 * |gravity| * m_jumpHeight): 6.0 m/s at height 1 and gravity 18, with no frame time in it.

  The loss is in the three frames after launch. FUN_104B0A20 casts downwards each frame and sets
  m_isSupported (input+0x31) for anything within ~3cm. The first airborne frame is 17.6cm up at
  30fps but only 0.86cm at 730fps, so hkpCharacterStateInAir::change (0x10BE5100) still sees
  support and lands a character rising at 6 m/s. The on-ground state then cancels vertical
  velocity, bounded only by hkpCharacterContext's m_maxLinearAcceleration (625) * dt clamp. Apex
  0.99m at 30fps, 0.38m at 730fps.

  The fix refuses that transition. m_isSupported is cleared across the one change() call while the
  character is rising faster than fRisingVelocity, then put back. The velocity is measured relative
  to the surface below, so a character riding a lift upwards still reads zero and still counts as
  landed. Clearing around the call instead of skipping it leaves the ladder transition on
  input+0x30 working. The suppression has no time or distance limit and needs none: gravity is
  integrated every frame whatever the state, so the upward velocity falls below the threshold
  within a fixed wall clock time at any framerate.

  Characters bounce uncontrollably at high framerates

  -norigidchars cures this by clearing one byte at [0x11613D60]+0x25, read only by FUN_10493CA0,
  which selects between two whole controllers:

        flag set (default)          alloc 0x100, ctor FUN_104B0F40, update FUN_104B1720
                                    -> hkpCharacterRigidBody, a dynamic body that exchanges
                                       impulses with the physics world
        flag clear (-norigidchars)  alloc 0x130, ctor FUN_104AE870, update FUN_104B0D20
                                    -> hkpCharacterProxy, swept through the world, never reacts

  It demotes every character to a kinematic proxy, which cannot be shoved by a vehicle or take
  momentum from an explosion. That is why it works and why it is not the fix.

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
  Implemented and measured, no effect. 625 * dt is already a rate; restoring the per FRAME budget
  would grant 6250 m/s^2 at 300fps and would loosen ordinary walking acceleration too. The frame
  quantised constant there is the hysteresis counter at controller+0xB4, left unpatched because
  nothing observed provokes it once the root motion fix below is in.

  NPCs bounce uncontrollably at high framerates

  FUN_100ACD10 turns animation root motion into a desired velocity as |displacement| / dt, clamped
  to 20. Root motion arrives on the animation's schedule and stays 30Hz sized as dt shrinks:
  0.024m per frame at 30fps against >0.060m at 332fps. The desired speed pins at the 20 m/s clamp
  on nearly every frame and the rigid body is driven at that speed while going nowhere, which is
  the bouncing. Flooring the divisor at 1/30 gives the speed 30fps would compute from the same
  displacement; at or below 30fps nothing changes, because the real frame time is already larger.

  None of the three has an ini key. They are bugs rather than preferences.
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

static SafetyHookInline InAirChangeHook{};
static SafetyHookInline CheckSupportHook{};
static SafetyHookMid RootMotionSpeedHook{};

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
                auto pattern = dunia_pattern("8B 44 24 08 80 78 31 00 74 14 8B 4C 24 0C 51 8B 4C 24 08 50 6A 00 E8 ? ? ? ? C2 0C 00 80 78 30 00");
                if (!pattern.empty())
                    InAirChangeHook = safetyhook::create_inline(pattern.get_first(), InAirChange);

                // hkpCharacterRigidBody::checkSupport. The two absolute addresses in its prologue
                // are wildcarded; what makes it unique is the run from the this-pointer save on.
                auto supportPattern = dunia_pattern("55 8B EC 83 E4 F0 81 EC 14 01 00 00 A1 ? ? ? ? 53 8B 1D ? ? ? ? 56 57 8B F1 50 89 74 24 4C FF D3 8B C8 8B 79 04");
                if (!supportPattern.empty())
                    CheckSupportHook = safetyhook::create_inline(supportPattern.get_first(), CheckSupport);

                // FUN_100ACD10, the root motion to desired velocity conversion. Matched from entry
                // through the two LEAs that set up the displacement out-parameters.
                auto rootMotionPattern = dunia_pattern("83 EC 1C 56 8B 74 24 24 8D 44 24 04 50 8D 4C 24 0C 51 8B CE E8 ? ? ? ? F3 0F 10 54 24 28");
                if (!rootMotionPattern.empty())
                    RootMotionSpeedHook = safetyhook::create_mid(rootMotionPattern.get_first(), RootMotionSpeed);
            };
    }
} FpsFixes;