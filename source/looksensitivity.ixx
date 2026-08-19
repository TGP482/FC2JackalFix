module;

#include <common.hxx>

export module looksensitivity;

import common;
import dunia;
import settings;

static float fMouseLookSensitivity = 1.0f;
static float fControllerLookSensitivity = 1.0f;

static bool bPadIsLookDevice = false;

static constexpr uintptr_t nFirstLookAxis = 6;
static constexpr uintptr_t nLastLookAxis = 7;

static float fSprintTurnModifier = 0.0f;

static constexpr size_t nSprintTurnSites = 2;

class LookSensitivity
{
public:
    LookSensitivity()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            BindFloat(fMouseLookSensitivity, PREF_MOUSELOOKSENSITIVITY);
            BindFloat(fControllerLookSensitivity, PREF_CONTROLLERLOOKSENSITIVITY);
            BindFloat(fSprintTurnModifier, PREF_SPRINTTURNMODIFIER);

            // Marks controller as the active look device when right stick input exceeds
            // the engine dead zone.
            if (auto* p = dunia_find("0F 28 C8 0F 54 CA F3 0F 10 51 08 56 0F 2F D1 0F 57 C9 57", 12))
            {
                static auto PadLookDeviceHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    if (regs.edx < nFirstLookAxis || regs.edx > nLastLookAxis)
                        return;

                    if (regs.xmm1.f32[0] > regs.xmm2.f32[0])
                        bPadIsLookDevice = true;
                });
            }

            // Marks mouse as the active look device when mouse movement is detected.
            if (auto* p = dunia_find("8B 44 24 08 F3 0F 2A 10 F3 0F 2A 58 04 83 EC 14 53 55 8B E9 0F 2E 55 18"))
            {
                static auto MouseLookDeviceHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto pAccumulator = *(int**)(regs.esp + 8);
                    if (pAccumulator == nullptr)
                        return;

                    if (pAccumulator[0] != 0 || pAccumulator[1] != 0)
                        bPadIsLookDevice = false;
                });
            }

            // Scales the loaded GameProfile sensitivity, leaving the in-game slider value alone.
            if (auto* p = dunia_find("F3 0F 10 88 B0 00 00 00 8B 88 B8 00 00 00 F3 0F 10 15 ? ? ? ? F3 0F 11 4C 24 08", 22))
            {
                static auto LookSensitivityHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    regs.xmm1.f32[0] *= bPadIsLookDevice ? fControllerLookSensitivity : fMouseLookSensitivity;
                });
            }

            // Overrides the sprint yaw modifier at both application sites. 0 keeps the archetype's.
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
