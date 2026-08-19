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
// CCameraShakeAndPadRumbleComponent drives both camera shake and pad rumble. Its archetype
// registers four curves at 0x101411D0: 0 archCamera_roll, 1 archCamera_pitch, 2 archRumble_HF
// (high frequency, small motor), 3 archRumble_LF (low frequency, large motor). Three concurrent
// effect instances live at base+0x14, stride 0x48; each carries one 0x10 byte entry per curve
// (+0x04 elapsed, +0x08 duration, +0x0C curve handle) and a live byte at +0x44. Every shake and
// rumble in the game is one CCameraShakeAndPadRumbleEvent landing in these slots.
//
// Both halves are present. 0x10141480 EvalCurve(this, index) returns the largest magnitude
// sample across the live instances and works for indices 2 and 3 as it stands. 0x104F07B0
// SetVibrationAll(this, float lf, float hf) enumerates vibration capable devices and calls
// vtable+0x20 on each, honouring its mute byte at this+0x85, which the in-game Vibration option
// and the pause and teardown silencing ride on. That reaches 0x102C8F80 CXInputPad::SetVibration,
// which scales both 0..1 amplitudes by 65535 into wLeftMotorSpeed/wRightMotorSpeed and calls
// XInputSetState.
//
// Missing is the join. 0x101417E0 is the per frame apply step:
//
//     101417E0  FLD   [ESP+4]              ; dt
//     101417E8  CALL  0x10141590           ; camera roll/pitch += Eval(0)/Eval(1) * dt * pi/4
//     101417ED  MOV   ECX,[0x11645578]     ; input device container
//     101417FB  CALL  0x104F3520           ; GetDevice(0)
//     10141804  FLDZ                       ; <- the amputation
//     10141809  FST   [ESP+4]              ; hf = 0.0
//     1014180F  FSTP  [ESP]                ; lf = 0.0
//     10141812  CALL  0x104FE590           ; rumble manager = *(device+4) + 0xE4
//     10141817  MOV   ECX,EAX
//     10141819  CALL  0x104F07B0           ; SetVibrationAll(0.0, 0.0)
//
// Curves 2 and 3 are never read anywhere in the binary, and all five callers of SetVibrationAll
// pass that same FLDZ pair. This substitutes the two rumble curve samples for the zeros one
// instruction before the dispatcher runs. The curves are authored game data shared with the 360,
// so nothing here scales magnitude.
//
// Departure from the console build: the motors only run while the pad is the active device, since
// shake events fire from the world rather than from the input and a plugged in pad would otherwise
// rumble through a mouse and keyboard session. IsPadActiveDevice from inputdevice watches the three
// raw input drivers rather than the action map, which by this point has forgotten which device
// produced anything. Narrowing it regresses this feature: while it tracked only the look axes, a
// pad went on rumbling through a mouse and keyboard firefight until the camera was swung.

static bool bVibration = true;

// CCameraShakeAndPadRumbleComponent::EvalCurve at 0x10141480.
using EvalCurve_t = float(__thiscall*)(void* self, int nCurveIndex);
static EvalCurve_t EvalCurve = nullptr;

// Archetype curve indices.
static constexpr int nCurveRumbleHF = 2;
static constexpr int nCurveRumbleLF = 3;

// Entry of 0x101417E0 to the MOV ECX,EAX before the dispatcher call. ESP still points at
// the two motor arguments and both zeros are written by then.
static constexpr ptrdiff_t nMotorArgs = 0x37;

// Sampled at the entry of 0x101417E0, consumed at nMotorArgs. The evaluator's this is not
// preserved across the camera call. Curve time only advances in the caller's tick loop
// after this returns, so both points see identical curve state.
static float fMotorLF = 0.0f;
static float fMotorHF = 0.0f;

// The clamp changes nothing the player can feel: SetVibration pins anything outside 0..1 once
// it has scaled by 65535. The isfinite check is the real guard. A NaN passes both clamp
// comparisons and reaches XInputSetState, where CVTTSS2SI turns it into 0x80000000, clamping
// one motor to zero while the other keeps running.
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
            auto* pEvaluator = dunia_find("83 EC 0C 0F 57 C0 53 55 8B E9 56 8D 45 58 57 F3 0F 11 44 24 14 33 DB");
            if (!pEvaluator)
                return;

            EvalCurve = reinterpret_cast<EvalCurve_t>(pEvaluator);

            // Anchored on the prologue, unique to 0x101417E0. The other four callers of
            // SetVibrationAll share the FLDZ block but not this entry, so shutdown, level
            // unload, pause and the destructor keep sending real zeros.
            auto apply = dunia_pattern("D9 44 24 04 51 D9 1C 24 E8 ? ? ? ? 8B 0D ? ? ? ? 83 79 08 00 76 25 6A 00");
            if (apply.empty())
                return;

            static auto ShakeEntryHook = safetyhook::create_mid(apply.get_first(0), [](SafetyHookContext& regs)
            {
                // Fall through with zeros so a pad that stops being the active device goes
                // quiet next frame.
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
                // Both slots hold 0.0 at this point. First arg is the left motor, second
                // the right.
                *reinterpret_cast<float*>(regs.esp) = fMotorLF;
                *reinterpret_cast<float*>(regs.esp + 4) = fMotorHF;
            });

            BindBool(bVibration, PREF_VIBRATION);
        };
    }
} Vibration;
