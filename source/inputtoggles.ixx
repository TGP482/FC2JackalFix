module;

#include <common.hxx>

export module inputtoggles;

import common;
import dunia;
import settings;
import inputdevice;

// Aim down sights and sprint are hold-the-button in stock Far Cry 2. The movement controller's
// action dispatcher gets separate actions for the button going down and coming back up:
//
//   ironsight down  OR  byte [state+0x04], 0x08
//   ironsight up    AND byte [state+0x04], 0xF7
//   sprint down     OR  byte [state+0x04], 0x40
//   sprint up       AND byte [state+0x04], 0xBF  and end the run
//
// Nothing else in the pawn state touches those bits, so a toggle is just swallowing the "up"
// action when the press was short enough to count as a tap.
//
// State block hangs off pawn+0x10:
//
//   +0x140  input state    flags byte at +0x04: 0x08 ironsight, 0x40 sprint requested
//   +0x2D0  current state  flags byte at +0x04: 0x40 actually sprinting
//
// The request bit is cleared by the movement update every frame; a run already under way is
// carried by the current-state bit, which the engine drops whenever the run ends.
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

// A press shorter than this latches the toggle. Longer presses behave as stock.
static constexpr uint64_t nTapTime = 250;

// Sprint is keyboard only: the pad binds sprint to SprintLock instead of a press/release pair, so
// it already toggles in the stock game. Aim is toggled on both devices.
struct ToggleState
{
    bool bEngaged;
    bool bPressActive;
    bool bPressedWhileEngaged;
    uint64_t nPressTime;
};

static ToggleState AimState;
static ToggleState SprintState;

// Pawn state block from the movement controller. Every step can be null with no pawn.
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

    // Unclear from the disassembly whether the action repeats while held, so ignore repeats.
    if (state.bPressActive)
        return;

    state.bPressActive = true;
    state.nPressTime = GetTickCount64();

    // A press while latched always drops the toggle, tap or hold, so decide it here.
    state.bPressedWhileEngaged = state.bEngaged;
}

// Returns true when the engine's "button came up" handling should be skipped.
static bool OnRelease(ToggleState& state, bool bEnabled, bool bMayLatch)
{
    if (!bEnabled)
    {
        Reset(state);
        return false;
    }

    // A release with no press behind it is a repeat. Keep swallowing it while latched.
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

            // Button down. ECX already holds the pawn, left there by the dispatcher's own guard.
            //
            // 101448A1  CMP   EAX,[0x10F93A94]          ; ironsight down
            // 101448A9  CALL  0x1007E1B0                <- hook
            // 101448AE  OR    byte ptr [EAX+0x4],0x8
            if (auto* pAimPress = dunia_find("E8 ? ? ? ? 80 48 04 08 5F 5E 83 C4 10 C2 08 00"))
            {
                static auto AimPressHook = safetyhook::create_mid(pAimPress, [](SafetyHookContext&)
                {
                    OnPress(AimState, IsPadActiveDevice() ? bAimToggleController : bAimToggle);
                });
            }

            // Button up. Skipping to the epilogue leaves the ironsight bit set.
            //
            // 1014492D  CMP   EAX,[0x10F93A98]          ; ironsight up
            // 10144935  CALL  0x1007E1B0                <- hook
            // 1014493A  AND   byte ptr [EAX+0x4],0xF7
            // 1014493E  POP   EDI                       <- join
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

            // 101447B5  CMP   EAX,[0x10F93A74]          ; sprint lock, shares the code below
            // 101447C1  CALL  0x1007E1B0                <- hook, also reached for [0x10F93A70]
            // 101447C6  OR    byte ptr [EAX+0x4],0x40
            if (auto* pSprintPress = dunia_find("E8 ? ? ? ? 80 48 04 40 5F 5E 83 C4 10 C2 08 00"))
            {
                static auto SprintPressHook = safetyhook::create_mid(pSprintPress, [](SafetyHookContext&)
                {
                    OnPress(SprintState, !IsPadActiveDevice() && bSprintToggle);
                });
            }

            // 101447D2  CMP   EAX,[0x10F93A78]          ; sprint up
            // 101447DA  CALL  0x1007E1B0                <- hook
            // 101447DF  AND   byte ptr [EAX+0x4],0xBF
            // 101447E5  CALL  0x10143650                ; end the run
            // 101447EA  POP   EDI                       <- join
            auto sprintReleasePattern = dunia_pattern("75 18 E8 ? ? ? ? 80 60 04 BF 8B CE E8 ? ? ? ? 5F 5E 83 C4 10 C2 08 00");
            if (!sprintReleasePattern.empty())
            {
                static auto nSprintReleaseJoin = reinterpret_cast<uintptr_t>(sprintReleasePattern.get_first(18));

                static auto SprintReleaseHook = safetyhook::create_mid(sprintReleasePattern.get_first(2), [](SafetyHookContext& regs)
                {
                    // Only latch a run that is actually happening. Tapping while standing still
                    // would arm a sprint that starts on the next step and never ends.
                    auto nBlock = StateBlock(regs.esi);
                    auto bSprinting = nBlock != 0
                        && (*(uint8_t*)(nBlock + nCurrentState + nStateFlags) & nSprintFlag) != 0;

                    auto bSuppress = OnRelease(SprintState, !IsPadActiveDevice() && bSprintToggle, bSprinting);

                    if (bSuppress)
                        regs.eip = nSprintReleaseJoin;
                });
            }

            // Once a frame the controller drops the state for any action no longer in the action
            // map on the stack, which is how the sights come down on entering a vehicle or menu.
            // Nothing is suppressed here; it just notices the engine ended the state itself.
            //
            // 10143B40  TEST  AL,AL                     <- hook, AL = action is bound in this map
            // 10143B44  MOV   ECX,[ESI+0x20]
            // 10143B4C  AND   byte ptr [EAX+0x4],0xF7
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

                    // The run ended on its own terms. Sprinting again takes another tap.
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
