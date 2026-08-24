module;

#include <common.hxx>
#include <algorithm>

export module vibration;

import common;
import dunia;
import inputdevice;
import settings;

// Controller vibration, restored from the Xbox 360 build.
static bool bVibration = true;

// CCameraShakeAndPadRumbleComponent::EvalCurve.
using EvalCurve_t = float(__thiscall*)(void* self, int nCurveIndex);
static EvalCurve_t EvalCurve = nullptr;

// Archetype curve indices.
static constexpr int nCurveRumbleHF = 2;
static constexpr int nCurveRumbleLF = 3;

// Entry to the MOV ECX,EAX before the dispatcher call; ESP still points at the two motor args.
static constexpr ptrdiff_t nMotorArgs = 0x37;

// Sampled at entry, consumed at nMotorArgs: the evaluator's this is not preserved across the camera
// call. Curve time advances only after this returns, so both points see the same curve state.
static float fMotorLF = 0.0f;
static float fMotorHF = 0.0f;

// A NaN passes both clamp comparisons and reaches XInputSetState, where CVTTSS2SI turns it into
// 0x80000000 and pins one motor to zero; the isfinite check is the real guard.
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

            // Anchored on the prologue, unique to this caller: the other four SetVibrationAll callers
            // share the FLDZ block, so shutdown, unload, pause and the destructor still send real zeros.
            auto apply = dunia_pattern("D9 44 24 04 51 D9 1C 24 E8 ? ? ? ? 8B 0D ? ? ? ? 83 79 08 00 76 25 6A 00");
            if (apply.empty())
                return;

            static auto ShakeEntryHook = safetyhook::create_mid(apply.get_first(0), [](SafetyHookContext& regs)
            {
                // Fall through with zeros so a pad that stops being the active device goes quiet.
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
                // Both slots hold 0.0 here. First arg left motor, second right.
                *reinterpret_cast<float*>(regs.esp) = fMotorLF;
                *reinterpret_cast<float*>(regs.esp + 4) = fMotorHF;
            });

            BindBool(bVibration, PREF_VIBRATION);
        };
    }
} Vibration;
