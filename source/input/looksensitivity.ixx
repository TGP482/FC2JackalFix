module;

#include <common.hxx>

export module looksensitivity;

import common;
import dunia;
import inputdevice;
import settings;

static float fMouseLookSensitivity = 1.0f;
static float fControllerLookSensitivity = 1.0f;

static bool bPadIsLookDevice = false;

static constexpr uintptr_t nFirstLookAxis = 6;
static constexpr uintptr_t nLastLookAxis = 7;

static float fSprintTurnModifier = 0.0f;

static constexpr size_t nSprintTurnSites = 2;

// Pad aim assist: four helpers, all applied from CActionMapPadFilter, so one gate disables all.
static bool bAimAssist = true;

static constexpr size_t nAimAssistSites = 4;

// Helper block is 0x1E bytes; offsets run from the CMP to the block and its end.
static constexpr ptrdiff_t nHelperBlockStart = 9;
static constexpr ptrdiff_t nHelperBlockEnd = 0x1E;

// One pattern per site: the blocks differ in scratch register.
static constexpr std::array<std::string_view, nAimAssistSites> sAimAssistPatterns =
{
    "80 BE 5D 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 4C 24 24 51 8B CE E8 ? ? ? ?", // stickyHelper
    "80 BE 5C 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 54 24 24 52 8B CE E8 ? ? ? ?", // followEnemyHelper
    "80 BE 5F 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 44 24 24 50 8B CE E8 ? ? ? ?", // ShootCorrectionHelper
    "80 BE 5E 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 4C 24 24 51 8B CE E8 ? ? ? ?", // IronSightHelper
};

static uintptr_t nHelperJoin[nAimAssistSites] = {};

// Skip the helper rather than clear its enable flag, which is reused elsewhere. Skipped for the
// mouse too: the filter runs off the action map, so with a pad plugged in it pulls the mouse's aim.
template <size_t nSite>
static void SkipAimAssistHelper(SafetyHookContext& regs)
{
    if (!bAimAssist || !IsPadActiveDevice())
        regs.eip = nHelperJoin[nSite];
}

// Hooked independently so one pattern failure does not lose the other three.
template <size_t nSite>
static void InstallAimAssistSkip()
{
    auto pattern = dunia_pattern(sAimAssistPatterns[nSite]);
    if (pattern.empty())
        return;

    nHelperJoin[nSite] = reinterpret_cast<uintptr_t>(pattern.get_first(nHelperBlockEnd));

    static auto AimAssistHook = safetyhook::create_mid(pattern.get_first(nHelperBlockStart), SkipAimAssistHelper<nSite>);
}

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

            InstallAimAssistSkip<0>();
            InstallAimAssistSkip<1>();
            InstallAimAssistSkip<2>();
            InstallAimAssistSkip<3>();

            BindBool(bAimAssist, PREF_AIMASSIST);

            // Right stick past the engine dead zone marks the pad as the look device.
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

            // Mouse movement marks the mouse as the look device.
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
