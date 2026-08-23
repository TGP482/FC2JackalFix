module;

#include <common.hxx>

export module inputtoggles;

import common;
import dunia;
import settings;
import inputdevice;

// Aim and sprint are hold-to-use in stock. Separate down/up actions set and clear bits in
// byte [state+0x04]: ironsight 0x08, sprint 0x40 (sprint up also ends the run). Nothing else
// touches those bits, so a toggle just swallows the "up" action after a tap.
//
// State block at pawn+0x10: +0x140 input state (0x40 = sprint requested, cleared every frame),
// +0x2D0 current state (0x40 = actually sprinting, carries a run already under way).
static constexpr uintptr_t nControllerPawn = 0x20;
static constexpr uintptr_t nPawnStateBlock = 0x10;

static constexpr uintptr_t nInputState = 0x140;
static constexpr uintptr_t nCurrentState = 0x2D0;
static constexpr uintptr_t nStateFlags = 0x04;

static constexpr uint8_t nIronsightFlag = 0x08;
static constexpr uint8_t nSprintFlag = 0x40;

static bool bAimToggle = false;
static bool bAimToggleController = false;
static bool bSprintToggle = false;

// Presses shorter than this latch the toggle.
static constexpr uint64_t nTapTime = 250;

// Sprint is keyboard only: the pad binds SprintLock, which already toggles.
struct ToggleState
{
    bool bEngaged;
    bool bPressActive;
    bool bPressedWhileEngaged;
    uint64_t nPressTime;
};

static ToggleState AimState;
static ToggleState SprintState;

static uintptr_t StateBlock(uintptr_t nController)
{
    if (nController == 0)
        return 0;

    auto nPawn = *(uintptr_t*)(nController + nControllerPawn);
    if (nPawn == 0)
        return 0;

    return *(uintptr_t*)(nPawn + nPawnStateBlock);
}

static void Reset(ToggleState& state)
{
    state.bEngaged = false;
    state.bPressActive = false;
}

static void OnPress(ToggleState& state, bool bEnabled)
{
    if (!bEnabled)
    {
        Reset(state);
        return;
    }

    if (state.bPressActive)
        return;

    state.bPressActive = true;
    state.nPressTime = GetTickCount64();

    // A press while latched drops the toggle, tap or hold.
    state.bPressedWhileEngaged = state.bEngaged;
}

// True to skip the engine's button-up handling.
static bool OnRelease(ToggleState& state, bool bEnabled, bool bMayLatch)
{
    if (!bEnabled)
    {
        Reset(state);
        return false;
    }

    // Release with no press is a repeat; keep swallowing while latched.
    if (!state.bPressActive)
        return state.bEngaged;

    state.bPressActive = false;

    if (state.bPressedWhileEngaged)
    {
        state.bEngaged = false;
        return false;
    }

    state.bEngaged = bMayLatch && (GetTickCount64() - state.nPressTime) <= nTapTime;

    return state.bEngaged;
}

class InputToggles
{
public:
    InputToggles()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            BindBool(bAimToggle, PREF_AIMTOGGLE);
            BindBool(bSprintToggle, PREF_SPRINTTOGGLE);
            BindBool(bAimToggleController, PREF_AIMTOGGLECONTROLLER);

            // Ironsight down: 101448A9 CALL 0x1007E1B0, before OR byte [EAX+0x4],0x8.
            if (auto* pAimPress = dunia_find("E8 ? ? ? ? 80 48 04 08 5F 5E 83 C4 10 C2 08 00"))
            {
                static auto AimPressHook = safetyhook::create_mid(pAimPress, [](SafetyHookContext&)
                {
                    OnPress(AimState, IsPadActiveDevice() ? bAimToggleController : bAimToggle);
                });
            }

            // Ironsight up: 10144935 CALL 0x1007E1B0. The join at 1014493E skips
            // AND byte [EAX+0x4],0xF7, leaving the bit set.
            auto aimReleasePattern = dunia_pattern("75 11 E8 ? ? ? ? 80 60 04 F7 5F 5E 83 C4 10 C2 08 00");
            if (!aimReleasePattern.empty())
            {
                static auto nAimReleaseJoin = reinterpret_cast<uintptr_t>(aimReleasePattern.get_first(11));

                static auto AimReleaseHook = safetyhook::create_mid(aimReleasePattern.get_first(2), [](SafetyHookContext& regs)
                {
                    auto bSuppress = OnRelease(AimState, IsPadActiveDevice() ? bAimToggleController : bAimToggle, true);

                    if (bSuppress)
                        regs.eip = nAimReleaseJoin;
                });
            }

            // Sprint down: 101447C1 CALL 0x1007E1B0, before OR byte [EAX+0x4],0x40. Reached
            // for both [0x10F93A70] and the sprint lock action [0x10F93A74].
            if (auto* pSprintPress = dunia_find("E8 ? ? ? ? 80 48 04 40 5F 5E 83 C4 10 C2 08 00"))
            {
                static auto SprintPressHook = safetyhook::create_mid(pSprintPress, [](SafetyHookContext&)
                {
                    OnPress(SprintState, !IsPadActiveDevice() && bSprintToggle);
                });
            }

            // Sprint up: 101447DA CALL 0x1007E1B0. The join at 101447EA skips
            // AND byte [EAX+0x4],0xBF and the end-of-run call 0x10143650.
            auto sprintReleasePattern = dunia_pattern("75 18 E8 ? ? ? ? 80 60 04 BF 8B CE E8 ? ? ? ? 5F 5E 83 C4 10 C2 08 00");
            if (!sprintReleasePattern.empty())
            {
                static auto nSprintReleaseJoin = reinterpret_cast<uintptr_t>(sprintReleasePattern.get_first(18));

                static auto SprintReleaseHook = safetyhook::create_mid(sprintReleasePattern.get_first(2), [](SafetyHookContext& regs)
                {
                    // Tapping while standing still would arm a sprint that never ends.
                    auto nBlock = StateBlock(regs.esi);
                    auto bSprinting = nBlock != 0
                        && (*(uint8_t*)(nBlock + nCurrentState + nStateFlags) & nSprintFlag) != 0;

                    auto bSuppress = OnRelease(SprintState, !IsPadActiveDevice() && bSprintToggle, bSprinting);

                    if (bSuppress)
                        regs.eip = nSprintReleaseJoin;
                });
            }

            // Once a frame the controller drops state for actions no longer in the action map,
            // which is how the sights come down on entering a vehicle or menu. Nothing is
            // suppressed here. Hook at 10143B40 TEST AL,AL; AL = action bound in this map.
            if (auto* pAimScope = dunia_find("84 C0 75 0C 8B 4E 20 E8 ? ? ? ? 80 60 04 F7"))
            {
                static auto AimScopeHook = safetyhook::create_mid(pAimScope, [](SafetyHookContext& regs)
                {
                    if (!AimState.bEngaged)
                        return;

                    auto nBlock = StateBlock(regs.esi);

                    if ((regs.eax & 0xFF) == 0 || nBlock == 0
                        || (*(uint8_t*)(nBlock + nInputState + nStateFlags) & nIronsightFlag) == 0)
                    {
                        Reset(AimState);
                    }
                });
            }

            if (auto* pSprintScope = dunia_find("84 C0 75 13 8B 4E 20 E8 ? ? ? ? 80 60 04 BF 8B CE"))
            {
                static auto SprintScopeHook = safetyhook::create_mid(pSprintScope, [](SafetyHookContext& regs)
                {
                    if (!SprintState.bEngaged)
                        return;

                    auto nBlock = StateBlock(regs.esi);

                    // The run ended on its own; sprinting again takes another tap.
                    if ((regs.eax & 0xFF) == 0 || nBlock == 0
                        || (*(uint8_t*)(nBlock + nCurrentState + nStateFlags) & nSprintFlag) == 0)
                    {
                        Reset(SprintState);
                    }
                });
            }
        };
    }
} InputToggles;
