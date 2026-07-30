/*
  Framerate dependent behaviour that the PC port inherited from a 30fps console target.

  ---------------------------------------------------------------------------------------------
  Jump height collapses at high framerates

  Symptom: the higher the framerate, the lower the player jumps. Measured on the stock game, one
  jump, player fJumpHeight 1.0 and fGravity -18:

        30 fps      launch 6.0000 m/s      apex 0.9865 m      airtime 1.233 s
       730 fps      launch 6.0002 m/s      apex 0.3772 m      airtime 0.424 s

  The launch velocity is identical. Everything is lost in the three frames after it.

  ---------------------------------------------------------------------------------------------
  How the jump is built

  CPawnData+0x4B0 is the live jump height, seeded from the entity library's fJumpHeight at spawn
  (which is why the modding guide says a new game is needed for that stat to take). Each frame
  FUN_100847C0 packs it, the desired velocity and a "jumped this frame" bit out of CPawnData+0x2D4
  into a parameter block and hands it to the character controller, which at FUN_104AF2E0 does:

        input->m_wantJump = params.bJumped;                    // controller+0x6C, +8
        if (input->m_wantJump)
            jumpingState->m_jumpHeight = params.JumpHeight;    // controller+0x5C, +8

  From there it is stock Havok. hkpCharacterStateOnGround::change (0x10BE4BA0) sees m_wantJump and
  enters hkpCharacterStateJumping, whose update (0x10BE5430) launches with

        jumpVel = sqrt( 2 * |m_characterGravity| * m_jumpHeight )

  so with height 1 and gravity 18 the character leaves the ground at exactly 6 m/s, for an apex of
  exactly one metre. None of that has a frame time in it, and the logs confirm it: the launch is
  6.0000 at 30fps and 6.0002 at 730fps.

  ---------------------------------------------------------------------------------------------
  Where it actually goes wrong

  The character controller decides whether it is standing on something by casting downwards every
  frame in FUN_104B0A20 and writing the answer to hkpCharacterInput::m_isSupported at input+0x31.
  That cast reports support for anything within roughly three centimetres - measured from the logs,
  support was still reported 2.34cm above the launch and gone by 3.15cm.

  Three centimetres is a distance. A frame is a time. At 6 m/s:

        30 fps      first airborne frame is 17.65 cm up      six times clear of the tolerance
       730 fps      first airborne frame is  0.86 cm up      still inside it

  So at high framerates the character is still "supported" for the first few frames of its own
  jump, and hkpCharacterStateInAir::change (0x10BE5100) does what it is told:

        if (input.m_isSupported) { context.setState(HK_CHARACTER_ON_GROUND, ...); return; }

  It lands the character while it is travelling upwards at six metres per second. What follows is
  the on-ground state doing its job correctly on a premise that is false. From the 730fps log,
  three consecutive frames:

        row 3   in air -> on ground,  supported=1, rising 5.9756 m/s, 1.67cm up
        row 4   on ground update      5.9756 -> 4.9556    lost 1.0200
        row 5   on ground -> in air,  supported=0        4.9556 -> 3.5122    lost 1.4434

  Both losses are the on-ground state cancelling vertical velocity, and both are exactly what
  hkpCharacterContext::update's acceleration clamp permits in one frame. Row 4 loses
  625 * 0.001634 = 1.0200, the clamp's m_maxLinearAcceleration times the frame time, to the digit.
  Row 5 loses (dt * gain / accelMag) * accel = 1.4438 by the same arithmetic. The clamp is the only
  reason the jump survives at all; without it the velocity would be zeroed outright on row 4.

  The character then resumes a normal, correctly integrated ascent - from 3.5122 m/s instead of
  5.9756. 3.5122^2 / (2 * 18) = 0.343m, plus the 0.031m it had already climbed, is the 0.377m the
  log reports. The entire deficit is those three frames.

  This is why the bug scales with framerate rather than switching on at some threshold: the faster
  the game runs, the smaller the first step, the more frames elapse before the character clears
  three centimetres, and the more of them the on-ground state gets to cancel.

  ---------------------------------------------------------------------------------------------
  The fix

  A character that is moving upwards has not landed on anything. So the in-air to on-ground
  transition is refused while the character is still rising relative to whatever surface is under
  it, by clearing m_isSupported for the duration of that one decision and putting it back
  afterwards.

  Clearing the flag around the call rather than skipping the call outright matters: the same
  function also handles the ladder transition on input+0x30, and that has nothing to do with any of
  this and is left working.

  Relative to the surface, not absolute, so a character standing on a rising lift still has a
  relative velocity of zero and still counts as landed.

  The threshold sits between two well separated populations. From the logs, a character genuinely
  resting on the ground carries about 0.02 m/s of residual upward velocity from the proxy pushing
  itself out of penetration, and a jump launches at 6 m/s. Half a metre per second is twenty five
  times the former and a twelfth of the latter, and every landing in both logs arrives with a
  strongly negative velocity, so nothing near a real landing comes close to it.

  There is no distance or time limit on the suppression and none is needed: gravity is integrated
  every frame regardless of state, so the upward velocity is guaranteed to fall below the threshold
  within a fixed wall clock time no matter the framerate, and the character lands normally.

  The hook is on Havok's own state, not on anything player specific, so AI characters - which jump
  through exactly the same state machine - are fixed by the same change.

  Not toggleable. This is a bug, not a preference.

  Result at 730 fps: the three frames of cancellation do not happen, the character keeps the 5.9756
  m/s it launched with, and the apex returns to approximately one metre. The residual difference
  from the 30fps figure of 0.9865m is the coarse integration at 30fps slightly undershooting the
  closed form, not the fix overshooting it.

  ---------------------------------------------------------------------------------------------
  Characters bounce uncontrollably at high framerates

  The launch command -norigidchars cures this, and understanding what it does is most of the
  diagnosis. It clears one byte at [0x11613D60]+0x25, which FUN_10493CA0 is the only thing to read,
  and which selects between two entirely different character controllers:

        flag set (default)          alloc 0x100, ctor FUN_104B0F40, update FUN_104B1720
                                    -> hkpCharacterRigidBody, a real dynamic body in the physics
                                       world that exchanges impulses with it
        flag clear (-norigidchars)  alloc 0x130, ctor FUN_104AE870, update FUN_104B0D20
                                    -> hkpCharacterProxy, swept through the world, never reacts

  So the switch does not disable an effect, it demotes every character to a kinematic proxy. That
  is why it works and why it is not a fix: the proxy cannot be shoved by a vehicle, cannot take
  momentum from an explosion, and cannot push anything it walks into.

  ---------------------------------------------------------------------------------------------
  A support test measured in seconds instead of metres

  Every frame the rigid controller asks hkpCharacterRigidBody::checkSupport (0x10BE9F40) whether
  the character is standing on anything. checkSupport gathers the nearby contact planes and hands
  them to Havok's simplex solver along with a probe: a velocity of exactly one metre per second
  straight down, and the frame's delta time. The solver reports support if that probe gets
  obstructed. A probe of speed v over time dt reaches v * dt, and with v fixed at 1 the reach is
  numerically the frame time:

        30 fps      reach 33.3 mm
       360 fps      reach  2.58 mm

  A character rigid body does not rest exactly on the ground - Havok holds it off by its contact
  tolerance, and it drifts within that. Measured on a stationary NPC at 360fps, the ground planes
  under it sat between 0.49 mm and 5.23 mm away, and the body floated up to 5.8 mm above its own
  resting height. 5.8 mm is comfortably inside a 33.3 mm reach and comfortably outside a 2.58 mm
  one, so at 30fps support is never in doubt and at 360fps it is lost the moment the character
  drifts up by more than a couple of millimetres.

  What follows, measured on a badly affected NPC:

        support is lost while the character is plainly standing still
        the state machine therefore reports in-air, and applies gravity every frame
        the body sinks into the ground and the dynamic solver ejects it - one observed frame
            handed the body +8.6788 m/s that no part of the controller asked for
        the on-ground state tries to cancel that, but hkpCharacterContext's filter only permits
            m_maxLinearAcceleration * dt of change per frame - 625 * dt:
                row 129   7.0196 -> 5.4333    delta 1.5863 = 625 * 0.002540
                row 130   5.4333 -> 3.8947    delta 1.5386 = 625 * 0.002467
        at 30fps that budget is 20.8 m/s in a single frame, so an impulse like that is annihilated
            before the character can move. At 360fps it is 1.56 m/s, cancelling 8.68 m/s needs
            13.9 ms, and the controller's support hysteresis is three FRAMES - 7.6 ms - so the
            character is released into the air still carrying 2.28 m/s and sails up 1.3 metres

  Every step after the first is a consequence. The root is that a distance test was written as a
  time test.

  ---------------------------------------------------------------------------------------------
  The fix

  Give the probe the reach it has at 30fps, by clamping the delta time checkSupport hands the
  solver to a floor of 1/30 for the duration of that one call. The reach becomes 33.3 mm at every
  framerate, which is precisely the 30fps behaviour, and below 30fps nothing changes because the
  real frame time is already larger.

  The delta time is restored immediately afterwards, and that is safe rather than lucky:
  FUN_104B10D0 writes the frame time into the character input and calls checkSupport before
  anything else reads it, and the state machine's own integration does not run until
  FUN_104B1720 continues afterwards. Nothing but the probe ever sees the clamped value.

  checkSupport reads the delta time in exactly two places, both of them the simplex input's own
  copy, so the probe reach is the only thing this can affect. It is also specific to
  hkpCharacterRigidBody - the proxy has its own support test at 0x10BE7F80 - so this touches only
  the controller that is actually broken, and it fixes the player and the AI together because both
  use it.

  Not toggleable. This is a bug, not a preference.

  ---------------------------------------------------------------------------------------------
  A correction budget measured in frames - considered, and rejected

  With the support clamp above live the probe reach reads 0.03333 on every frame, and support
  switches on the 33.3 mm threshold instead of a 2.6 mm one, exactly as intended:

        nearest ground plane 0.03602 m   ->  unsupported
        nearest ground plane 0.02940 m   ->  supported

  Characters were still being launched, and the next suspect was hkpCharacterContext::update
  (0x10BE46D0), which filters the state machine's output through an acceleration clamp:

        accel   = (desired - current) * m_invDeltaTime
        if |accel| / m_maxLinearAcceleration > 1 and the state is not jumping,
            the change is limited to m_maxLinearAcceleration * m_deltaTime

  m_maxLinearAcceleration is 625, so the velocity a frame may remove is 625 * dt - 20.8 m/s at
  30fps, 1.6 m/s at 380fps. An unwanted impulse that 30fps annihilates inside the single frame it
  arrives on takes seven frames at 380fps, and the controller's support hysteresis is three FRAMES,
  so the character is handed back to the in-air state still carrying most of it and flies.

  Scaling m_maxLinearAcceleration by (1/30)/dt was implemented and measured against that reasoning.
  It made no difference, and on reflection it should not have:

    - 625 * dt is a rate, and a rate is already the framerate independent form. Restoring the per
      FRAME budget does not restore 30fps behaviour, it grants ten times the sustained acceleration
      30fps could ever produce - 6250 m/s^2 at 300fps against 625 m/s^2 at 30fps. It is a loosening
      wearing a fix's clothes, and it would apply to every character's ordinary walking
      acceleration, not just to unwanted impulses.
    - The genuinely frame-quantised constant in that interaction is the support hysteresis at
      controller+0xB4, which counts three FRAMES where it means to ride out a transient. That is a
      real defect of the same family as everything else in this file, but nothing observed provokes
      it any more, so it is recorded here rather than patched blind.
    - The impulses it was meant to absorb were themselves a symptom, not a cause. Their source is
      the root motion bug below, and with that fixed a standing NPC's vertical velocity spans
      6.5 mm/s across four seconds at 370fps. There is nothing left for the budget to annihilate.

  So it is not here. If character launching ever reappears, the hysteresis counter is the place to
  look, not the acceleration clamp.

  ---------------------------------------------------------------------------------------------
  NPCs bounce uncontrollably at high framerates

  The support clamp above is real and independently proven, and it is not this. The bouncing starts
  before any of that code runs, in the pawn's own movement update.

  FUN_100ACD10 turns the character's animation root motion into a desired velocity:

        FUN_10522010(&displacement, ...)              ; this frame's root motion
        speed = |displacement| / dt
        if (speed > 20) speed = 20
        desiredVelocity = normalise(displacement) * speed

  and FUN_100ACEA0 hands that to the character controller, which faithfully drives the rigid body
  with it. Measured on a standing NPC, same code path in both runs:

        30 fps    dt 0.0333   desired speed  0.728 m/s   body travels     0.5 mm
       332 fps    dt 0.0030   desired speed 20.000 m/s   body travels   672.5 mm

  The desired speed is not merely larger, it is pinned at the clamp on essentially every frame, and
  the body is being driven at up to 20 m/s while going nowhere - which is what the bouncing is. The
  vertical thrashing everything started from is this velocity being fought over by the contact
  solver.

  Working back from the numbers, the displacement is the giveaway:

        30 fps    speed 0.728 * dt 0.0333  =  0.0243 m of root motion per frame
       332 fps    speed >= 20  * dt 0.0030 = >0.0602 m of root motion per frame

  At eleven times the framerate the displacement is two and a half times LARGER. Correct per frame
  root motion would be eleven times smaller. A displacement of a few centimetres is a 30Hz sized
  step, and it stays that size no matter how short the frame gets - the animation produces root
  motion on its own schedule, and this code divides it by the renderer's frame time. At 30fps the
  two happen to agree and the arithmetic is right. Above that it is dividing a 33ms displacement by
  a 3ms frame, and the quotient runs away until it hits the clamp.

  This is the same shape as the other two fixes and as the jump: a quantity in metres divided by a
  frame time that no longer means what the divisor was written to assume.

  ---------------------------------------------------------------------------------------------
  The fix

  Floor the divisor at 1/30 for the duration of that one call. The speed becomes exactly what 30fps
  would compute from the same displacement, which is the definition of the behaviour being restored,
  and at or below 30fps nothing changes because the real frame time is already larger.

  A mid hook at the function's first instruction, rewriting the delta time argument in place on the
  stack, rather than an inline hook with a declared prototype: the argument slot belongs to this
  call alone, so nothing else can observe the substitution, and it sidesteps having to be right
  about the calling convention of a function that is only being nudged.

  Not toggleable. This is a bug, not a preference.

  Measured on a standing NPC after the fix, four second captures:

        30 fps    dt 0.0334   desired speed 0.00 - 4.64 m/s   vertical span 0.6 mm
       370 fps    dt 0.0027   desired speed 0.00 - 4.76 m/s   vertical span 2.1 mm

  The desired speed is no longer pinned at the 20 m/s clamp at any point in either run, it stays
  inside the pawn's own 5.0 walking maximum, and the 672.5 mm of vertical travel is gone. The 30fps
  figures are unchanged from the stock game, which is what a floor at 1/30 is supposed to mean.
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

// Metres per second of upward motion, relative to the surface below, at which the character is
// taken to be rising rather than landing.
static constexpr float fRisingVelocity = 0.5f;

// hkpStepInfo, as hkpCharacterRigidBody::checkSupport is handed it.
static constexpr uint32_t nStepInfoDeltaTime = 0x08;

// The frame time everything below is referenced to. The console build ran at 30fps and the
// movement constants were tuned against a frame of this length.
static constexpr float fReferenceDelta = 1.0f / 30.0f;

// The frame time the support probe is given, floored. The probe travels one metre per second, so
// this is also its reach in metres: 33.3 mm, the same as a stock 30fps frame.
static constexpr float fSupportProbeMinDelta = fReferenceDelta;

// The delta time argument to FUN_100ACD10, relative to ESP at its first instruction: return
// address, then the owner pointer, then the frame time.
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

// hkpCharacterStateInAir::change. this is unused by the original, and all three arguments are on
// the stack, but it is entered as a __thiscall so ECX has to be carried through the trampoline.
static void __fastcall InAirChange(void* pThis, void* pEdx, void* pContext, void* pInput, void* pOutput)
{
    auto* pSupported = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pInput) + nInputIsSupported);
    const auto nSupported = *pSupported;

    if (nSupported != 0 && UpwardVelocity(reinterpret_cast<uintptr_t>(pInput)) > fRisingVelocity)
        *pSupported = 0;

    InAirChangeHook.fastcall(pThis, pEdx, pContext, pInput, pOutput);

    // Restored unconditionally. The flag is read again later in the same frame - the two Far Cry 2
    // specific character states at controller+0x64 and +0x68 both branch on it - and it is rebuilt
    // from the downward cast at the top of the next frame regardless, so the only window this
    // needs to cover is the transition decision itself.
    *pSupported = nSupported;
}

// hkpCharacterRigidBody::checkSupport. The step info it is handed is the character input's own
// copy at input+0x70, so the delta time being written here is input+0x78 - which is why it is put
// back before returning. Between the write and the restore, nothing but the simplex probe reads it.
static void __fastcall CheckSupport(void* pThis, void* pEdx, uint8_t* pStepInfo, void* pSurface)
{
    auto* pDelta = reinterpret_cast<float*>(pStepInfo + nStepInfoDeltaTime);
    const auto fOriginal = *pDelta;

    if (fOriginal < fSupportProbeMinDelta)
        *pDelta = fSupportProbeMinDelta;

    CheckSupportHook.fastcall(pThis, pEdx, pStepInfo, pSurface);

    *pDelta = fOriginal;
}

// FUN_100ACD10, at its first instruction, where the arguments are still where the caller pushed
// them. Only the divisor is touched; the displacement and everything downstream are left alone.
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
            // hkpCharacterStateInAir::change, matched from its entry through the m_isSupported test
            // and the setState(HK_CHARACTER_ON_GROUND) it guards. The call displacement is
            // wildcarded; nothing else in the image reaches +0x31 of a stack argument and then
            // pushes a zero state id.
            auto pattern = dunia_pattern("8B 44 24 08 80 78 31 00 74 14 8B 4C 24 0C 51 8B 4C 24 08 50 6A 00 E8 ? ? ? ? C2 0C 00 80 78 30 00");
            if (!pattern.empty())
                InAirChangeHook = safetyhook::create_inline(pattern.get_first(), InAirChange);

            // hkpCharacterRigidBody::checkSupport. The two absolute addresses in its prologue are
            // wildcarded; what makes it unique is the run from the this-pointer save onwards.
            auto supportPattern = dunia_pattern("55 8B EC 83 E4 F0 81 EC 14 01 00 00 A1 ? ? ? ? 53 8B 1D ? ? ? ? 56 57 8B F1 50 89 74 24 4C FF D3 8B C8 8B 79 04");
            if (!supportPattern.empty())
                CheckSupportHook = safetyhook::create_inline(supportPattern.get_first(), CheckSupport);

            // FUN_100ACD10, the root motion to desired velocity conversion. Matched from its entry
            // through the two LEAs that set up the displacement out-parameters, with the call
            // displacement wildcarded.
            auto rootMotionPattern = dunia_pattern("83 EC 1C 56 8B 74 24 24 8D 44 24 04 50 8D 4C 24 0C 51 8B CE E8 ? ? ? ? F3 0F 10 54 24 28");
            if (!rootMotionPattern.empty())
                RootMotionSpeedHook = safetyhook::create_mid(rootMotionPattern.get_first(), RootMotionSpeed);
        };
    }
} FpsFixes;
