module;

#include <common.hxx>
#include <algorithm>

export module vibration;

import common;
import dunia;
import inputdevice;
import settings;

// Controller vibration, restored from the Xbox 360 build.
//
// Far Cry 2 drives camera shake and pad rumble from one data driven component,
// CCameraShakeAndPadRumbleComponent. Its archetype declares four animation curves,
// registered at 0x101411D0 in index order:
//
//     0  archCamera_roll
//     1  archCamera_pitch
//     2  archRumble_HF     high frequency, the small motor
//     3  archRumble_LF     low frequency, the large motor
//
// The component holds three concurrent effect instances at base+0x14, stride 0x48. Each
// instance carries one 0x10 byte entry per curve (+0x04 elapsed, +0x08 duration, +0x0C
// curve handle) and a live byte at +0x44.
//
// 0x10141480 is the evaluator. EvalCurve(this, index) walks the three instances, samples
// the requested curve on each live one, and returns the sample with the largest magnitude.
// Nothing about it is specific to the camera curves - it returns correct values for
// indices 2 and 3 as it stands.
//
// The output path is intact as well:
//
//     0x104F07B0  SetVibrationAll(this, float lf, float hf)
//                   enumerates every vibration capable device and calls vtable+0x20 on it.
//                   Honours its own mute byte at this+0x85, which is what the in-game
//                   Vibration option and the pause and teardown silencing ride on.
//     0x102C8F80  CXInputPad::SetVibration(this, float lf, float hf)
//                   lf * 65535 -> wLeftMotorSpeed, hf * 65535 -> wRightMotorSpeed, both
//                   clamped to 0..65535, then a real XInputSetState.
//
// So both motors take a normalised 0..1 amplitude, and the left/right split lines up with
// the LF/HF pair the archetype declares.
//
// What the PC build is missing is the join between the two halves. 0x101417E0 is the per
// frame "apply shake and rumble" step:
//
//     101417E0  FLD   [ESP+4]              ; dt
//     101417E4  PUSH  ECX                  ; this, immediately overwritten by the FSTP
//     101417E5  FSTP  [ESP]
//     101417E8  CALL  0x10141590           ; camera: roll  += Eval(0) * dt * pi/4
//                                          ;         pitch += Eval(1) * dt * pi/4
//     101417ED  MOV   ECX,[0x11645578]     ; input device container
//     101417F3  CMP   [ECX+8],0
//     101417FB  CALL  0x104F3520           ; GetDevice(0)
//     10141804  FLDZ                       ; <- the amputation
//     10141806  SUB   ESP,8
//     10141809  FST   [ESP+4]              ; hf = 0.0
//     1014180F  FSTP  [ESP]                ; lf = 0.0
//     10141812  CALL  0x104FE590           ; rumble manager = *(device+4) + 0xE4
//     10141817  MOV   ECX,EAX
//     10141819  CALL  0x104F07B0           ; SetVibrationAll(0.0, 0.0)
//
// It evaluates curves 0 and 1 for the camera and then hands the motors two literal zeros.
// Curves 2 and 3 are never read by anything in the binary. All five callers of
// SetVibrationAll are copies of that same FLDZ pair, which is why the only function able
// to set motor speeds is used exclusively to switch them off: the console build's
// arguments were replaced with constants rather than the calls being removed.
//
// Restoring it is therefore implementing it, not enabling it. This evaluates the two
// rumble curves with the game's own evaluator and substitutes the results for the zeros,
// one instruction before the dispatcher runs. Everything downstream is stock - the mute
// byte, the device enumeration, the 65535 scale, the clamp, XInputSetState - and the
// curves are authored game data shared with the 360, so the intensity is the console's by
// construction rather than by tuning. Because every shake and rumble in the game is one
// CCameraShakeAndPadRumbleEvent landing in these instance slots, weapons, explosions,
// vehicles and malaria attacks are all covered by the single substitution.
//
// One deliberate departure from the console build: the motors only run while the pad is
// the device the player is actually using. The 360 had no such condition because it had no
// alternative, but on PC a plugged in controller would otherwise buzz through a whole
// session played on mouse and keyboard - the shake events fire from the world, not from
// the input, so firing a gun with the mouse would rumble a pad nobody is holding.
//
// The test is IsPadActiveDevice() from inputdevice, the same one the aim and sprint
// toggles use to decide which of their two preferences applies. It watches the three raw
// input drivers rather than the action map, which by this point has forgotten which device
// produced anything, so every button, key, trigger and stick votes on who is playing.
// Getting that right matters more here than anywhere else in the fix: while it tracked
// only the look axes, a pad went on rumbling through a mouse and keyboard firefight until
// the player happened to swing the camera.

static bool bVibration = true;

// CCameraShakeAndPadRumbleComponent::EvalCurve at 0x10141480.
using EvalCurve_t = float(__thiscall*)(void* self, int nCurveIndex);
static EvalCurve_t EvalCurve = nullptr;

// Archetype curve indices, in the order 0x101411D0 registers them.
static constexpr int nCurveRumbleHF = 2;
static constexpr int nCurveRumbleLF = 3;

// Distance from the entry of 0x101417E0 to the MOV ECX,EAX that precedes the dispatcher
// call. ESP still points at the two motor arguments there, and both zeros have been
// written by then, so this is the last point at which they can be replaced.
static constexpr ptrdiff_t nMotorArgs = 0x37;

// Sampled at the entry of 0x101417E0 and consumed at nMotorArgs. The evaluator's this is
// not preserved across the camera call, so the values cannot be taken at the call site.
// The camera step that runs in between only writes the camera outputs - it never advances
// curve time, which happens in the caller's tick loop after this function returns - so
// both points see identical curve state.
static float fMotorLF = 0.0f;
static float fMotorHF = 0.0f;

// SetVibration pins anything outside 0..1 once it has scaled by 65535, so clamping here
// changes nothing the player can feel. It exists to keep a malformed curve from reaching
// XInputSetState as a NaN, which would survive CVTTSS2SI as 0x80000000 and clamp to zero
// on one motor while leaving the other running.
static float Amplitude(void* self, int nCurveIndex)
{
    if (!self || !EvalCurve)
        return 0.0f;

    auto fValue = EvalCurve(self, nCurveIndex);
    if (!std::isfinite(fValue))
        return 0.0f;

    return std::clamp(fValue, 0.0f, 1.0f);
}

class Vibration
{
public:
    Vibration()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            auto evaluator = dunia_pattern("83 EC 0C 0F 57 C0 53 55 8B E9 56 8D 45 58 57 F3 0F 11 44 24 14 33 DB");
            if (evaluator.empty())
                return;

            EvalCurve = reinterpret_cast<EvalCurve_t>(evaluator.get_first(0));

            // Anchored on the prologue, which is unique to 0x101417E0. The other four
            // callers of SetVibrationAll share the FLDZ block but not this entry, so
            // shutdown, level unload, pause and the destructor keep sending real zeros.
            auto apply = dunia_pattern("D9 44 24 04 51 D9 1C 24 E8 ? ? ? ? 8B 0D ? ? ? ? 83 79 08 00 76 25 6A 00");
            if (apply.empty())
                return;

            static auto ShakeEntryHook = safetyhook::create_mid(apply.get_first(0), [](SafetyHookContext& regs)
            {
                // Falling through with zeros rather than skipping the hook keeps the stock
                // path intact: the engine was going to send this pair of zeros anyway, so a
                // pad that stops being the active device mid effect is silenced on the next
                // frame instead of holding whatever amplitude it had.
                if (!bVibration || !IsPadActiveDevice())
                {
                    fMotorLF = 0.0f;
                    fMotorHF = 0.0f;
                    return;
                }

                auto self = reinterpret_cast<void*>(regs.ecx);
                fMotorLF = Amplitude(self, nCurveRumbleLF);
                fMotorHF = Amplitude(self, nCurveRumbleHF);
            });

            static auto MotorArgsHook = safetyhook::create_mid(apply.get_first(nMotorArgs), [](SafetyHookContext& regs)
            {
                // The engine has just stored 0.0 into both slots. The first argument is the
                // left motor and the second is the right, matching LF and HF.
                *reinterpret_cast<float*>(regs.esp) = fMotorLF;
                *reinterpret_cast<float*>(regs.esp + 4) = fMotorHF;
            });

            static auto VibrationCB = []()
            {
                bVibration = JackalFixSettings.GetInt(PREF_VIBRATION) != 0;
            };

            VibrationCB();

            JackalFix::onIniFileChange() += []()
            {
                VibrationCB();
            };
        };
    }
} Vibration;
