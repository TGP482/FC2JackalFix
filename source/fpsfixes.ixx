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

static SafetyHookInline InAirChangeHook{};

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
            if (pattern.empty())
                return;

            InAirChangeHook = safetyhook::create_inline(pattern.get_first(), InAirChange);
        };
    }
} FpsFixes;
