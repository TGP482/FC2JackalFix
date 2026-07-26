module;

#include <common.hxx>

export module looksensitivity;

import common;
import dunia;
import settings;

// The look action applies sensitivity for both mouse and gamepad after input has been unified.
// Scaling Sensitivity matches the in-game slider but removes its cap. 1.0 is stock.
static float fLookSensitivity = 1.0f;

// Scales yaw while sprinting. 0 leaves the archetype's authored value unchanged.
static float fSprintTurnModifier = 0.0f;

// Sprint turn speed is applied both when sprinting and when entering a sprint.
// Both sites must be patched.
static constexpr size_t nSprintTurnSites = 2;

class LookSensitivity
{
public:
    LookSensitivity()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            static auto LookSensitivityCB = []()
            {
                fLookSensitivity = JackalFixSettings.GetFloat(PREF_LOOKSENSITIVITY);
                fSprintTurnModifier = JackalFixSettings.GetFloat(PREF_SPRINTTURNMODIFIER);
            };

            LookSensitivityCB();

            JackalFix::onIniFileChange() += []()
            {
                LookSensitivityCB();
            };

            // Hook the stack spill so the scale applies on top of the in-game slider value.
            auto sensitivityPattern = dunia_pattern("F3 0F 10 88 B0 00 00 00 8B 88 B8 00 00 00 F3 0F 10 15 ? ? ? ? F3 0F 11 4C 24 08");
            if (!sensitivityPattern.empty())
            {
                static auto LookSensitivityHook = safetyhook::create_mid(sensitivityPattern.get_first(22), [](SafetyHookContext& regs)
                {
                    regs.xmm1.f32[0] *= fLookSensitivity;
                });
            }

            // The sprint turn modifier is applied at two identical sites, both of which must be patched.
            // Overwrite XMM0 instead of the source value so 0 preserves the game's value and INI changes
            // apply without a restart.
            auto sprintTurnPattern = dunia_pattern("F3 0F 10 80 A4 00 00 00 F3 0F 59 46 14 F3 0F 11 46 14");
            if (sprintTurnPattern.size() == nSprintTurnSites)
            {
                static auto SprintTurnLookHook = safetyhook::create_mid(sprintTurnPattern.get(0).get<void>(8), [](SafetyHookContext& regs)
                {
                    if (fSprintTurnModifier > 0.0f)
                        regs.xmm0.f32[0] = fSprintTurnModifier;
                });

                static auto SprintTurnEnterHook = safetyhook::create_mid(sprintTurnPattern.get(1).get<void>(8), [](SafetyHookContext& regs)
                {
                    if (fSprintTurnModifier > 0.0f)
                        regs.xmm0.f32[0] = fSprintTurnModifier;
                });
            }
        };
    }
} LookSensitivity;
