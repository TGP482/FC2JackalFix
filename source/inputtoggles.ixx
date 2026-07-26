module;

#include <common.hxx>
#include <fstream>
#include <format>

export module inputtoggles;

import common;
import dunia;
import settings;
import inputdevice;

// Aim down sights and sprint are both "hold the button" in stock Far Cry 2. Both are driven from the
// movement controller's action dispatcher, which receives a separate action for the button going
// down and for it coming back up:
//
//   ironsight down  OR  byte [state+0x04], 0x08
//   ironsight up    AND byte [state+0x04], 0xF7
//   sprint down     OR  byte [state+0x04], 0x40
//   sprint up       AND byte [state+0x04], 0xBF  and end the run
//
// Nothing else in the pawn state touches those bits, so a toggle is just a matter of swallowing the
// "up" action when the press was short enough to count as a tap.
//
// The state lives in a block hanging off pawn+0x10. Two halves of it matter, and their names come
// from the pawn's own property table, which registers SprintLock, selStance and WorldSpeed at
// offsets that line up with what the movement update reads:
//
//   +0x140  input state    flags byte at +0x04: bit 0x08 ironsight, bit 0x40 sprint requested
//   +0x2D0  current state  flags byte at +0x04: bit 0x40 actually sprinting
//
// The sprint request bit is consumed and cleared by the movement update every frame; a run already
// under way is carried by the current-state bit instead. That bit is what the engine drops when the
// run legitimately ends - the player stops pushing forward, strafes too hard, aims, fires, or gets
// into a vehicle - so watching it gives "tap again once you stop" without enumerating the cases.
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
static bool bSprintToggleController = false;

// A press shorter than this latches the toggle. Anything longer is left alone, so holding the button
// behaves exactly as it does in the stock game. Comfortably above a deliberate tap and below the
// shortest press anyone makes when they mean to hold, so there is nothing here worth exposing.
static constexpr uint64_t nTapTime = 250;

// -------------------------------------------------------------------------------------------------
// TEMPORARY. The gamepad binds sprint to the SprintLock action rather than the keyboard's press and
// release pair, which is why a pad has always sprinted on a tap. What the disassembly cannot settle
// is what a pad sends when the button comes back up: nothing at all, the keyboard's release action,
// or one of the two other actions that also end a run. This records that and changes no behavior.
// Remove once the answer is in.
// -------------------------------------------------------------------------------------------------
static constexpr uintptr_t nSprintActionRva = 0xF93A70;
static constexpr uintptr_t nSprintLockActionRva = 0xF93A74;
static constexpr uintptr_t nSprintUpActionRva = 0xF93A78;
static constexpr uintptr_t nOtherStopActionRva = 0xF93A7C;
static constexpr uintptr_t nOtherStopAction2Rva = 0xF93A90;

static constexpr size_t nDiagLineLimit = 800;

static bool bDiagLockPress = false;

static void Diag(const std::string& sLine)
{
    static size_t nLines = 0;
    if (nLines++ >= nDiagLineLimit)
        return;

    static const auto modulePath = []
    {
        static const auto nAnchor = 1;
        WCHAR buffer[MAX_PATH]{};
        HMODULE hModule = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&nAnchor, &hModule);
        GetModuleFileNameW(hModule, buffer, ARRAYSIZE(buffer));

        return std::filesystem::path(buffer);
    }();

    // Naming the .asi that wrote this makes a stale copy loading from somewhere else obvious, rather
    // than looking like a fix that did not work.
    static std::ofstream file = []
    {
        std::ofstream stream(std::filesystem::path(modulePath).replace_extension(".log"), std::ios::trunc);
        stream << "module " << modulePath.string() << std::endl;

        return stream;
    }();

    file << std::format("{:>7} ", GetTickCount64() % 1000000) << sLine << std::endl;
}

static uint32_t DiagAction(uintptr_t nRva)
{
    return *(uint32_t*)(reinterpret_cast<uintptr_t>(hDunia) + nRva);
}

static const char* DiagActionName(uint32_t nId)
{
    if (nId == DiagAction(nSprintActionRva))      return "Sprint(a70)";
    if (nId == DiagAction(nSprintLockActionRva))  return "SprintLock(a74)";
    if (nId == DiagAction(nSprintUpActionRva))    return "SprintUp(a78)";
    if (nId == DiagAction(nOtherStopActionRva))   return "Stop(a7c)";
    if (nId == DiagAction(nOtherStopAction2Rva))  return "Stop(a90)";

    return "unknown";
}

struct ToggleState
{
    bool bEngaged;
    bool bPressActive;
    bool bPressedWhileEngaged;
    uint64_t nPressTime;
};

static ToggleState AimState;
static ToggleState SprintState;

// Resolves the pawn's state block from the movement controller. Every step can be null while the
// player has no pawn, which happens across loads and cutscenes.
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

    // Whether the engine repeats this action while the button is down is not something the
    // disassembly settles, so ignore repeats and keep the time of the first one.
    if (state.bPressActive)
        return;

    state.bPressActive = true;
    state.nPressTime = GetTickCount64();

    // A press made while the toggle is latched is always a request to drop it, whether the player
    // taps or holds, so the outcome is decided here rather than on the release.
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

    // A release with no press behind it is a repeat of one already dealt with. Keep swallowing it
    // while latched rather than letting it undo the toggle.
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
            static auto InputTogglesCB = []()
            {
                bAimToggle = JackalFixSettings.GetInt(PREF_AIMTOGGLE) != 0;
                bSprintToggle = JackalFixSettings.GetInt(PREF_SPRINTTOGGLE) != 0;
                bAimToggleController = JackalFixSettings.GetInt(PREF_AIMTOGGLECONTROLLER) != 0;
                bSprintToggleController = JackalFixSettings.GetInt(PREF_SPRINTTOGGLECONTROLLER) != 0;
            };

            InputTogglesCB();

            JackalFix::onIniFileChange() += []()
            {
                InputTogglesCB();
            };

            // Button down. ECX already holds the pawn, left there by the dispatcher's own guard.
            //
            // 101448A1  CMP   EAX,[0x10F93A94]          ; ironsight down
            // 101448A7  JNZ   0x101448BA
            // 101448A9  CALL  0x1007E1B0                <- hook
            // 101448AE  OR    byte ptr [EAX+0x4],0x8
            auto aimPressPattern = dunia_pattern("E8 ? ? ? ? 80 48 04 08 5F 5E 83 C4 10 C2 08 00");
            if (!aimPressPattern.empty())
            {
                static auto AimPressHook = safetyhook::create_mid(aimPressPattern.get_first(), [](SafetyHookContext&)
                {
                    OnPress(AimState, IsPadActiveDevice() ? bAimToggleController : bAimToggle);
                });
            }

            // Button up. Skipping to the epilogue leaves the ironsight bit set, which is the whole
            // trick - nothing else in the pawn state clears it.
            //
            // 1014492D  CMP   EAX,[0x10F93A98]          ; ironsight up
            // 10144933  JNZ   0x10144946
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
            // 101447BD  MOV   byte ptr [ESI+0x18],0x1
            // 101447C1  CALL  0x1007E1B0                <- hook, also reached for [0x10F93A70]
            // 101447C6  OR    byte ptr [EAX+0x4],0x40
            auto sprintPressPattern = dunia_pattern("E8 ? ? ? ? 80 48 04 40 5F 5E 83 C4 10 C2 08 00");
            if (!sprintPressPattern.empty())
            {
                static auto SprintPressHook = safetyhook::create_mid(sprintPressPattern.get_first(), [](SafetyHookContext&)
                {
                    Diag(std::format("sprint down, lock={} pad={}", bDiagLockPress, IsPadActiveDevice()));
                    bDiagLockPress = false;

                    OnPress(SprintState, IsPadActiveDevice() ? bSprintToggleController : bSprintToggle);
                });
            }

            // TEMPORARY. Only the SprintLock action passes through here, so this says which of the
            // two sprint-down actions the press came from.
            //
            // 101447BD  MOV   byte ptr [ESI+0x18],0x1   <- hook
            // 101447C1  CALL  0x1007E1B0
            auto sprintLockPattern = dunia_pattern("C6 46 18 01 E8 ? ? ? ? 80 48 04 40 5F 5E 83 C4 10 C2 08 00");
            if (!sprintLockPattern.empty())
            {
                static auto SprintLockDiagHook = safetyhook::create_mid(sprintLockPattern.get_first(), [](SafetyHookContext&)
                {
                    bDiagLockPress = true;
                });
            }

            // TEMPORARY. Two further actions end a run through this shared call. EAX still holds the
            // action id, so this says whether a pad release arrives here and which action it is.
            //
            // 1014497B  MOV   ECX,ESI                   <- hook
            // 1014497D  CALL  0x10143650                ; end the run
            auto sprintStopPattern = dunia_pattern("8B CE E8 ? ? ? ? 8B 17 51 8B C4 89 10 8B 4E 20");
            if (!sprintStopPattern.empty())
            {
                static auto SprintStopDiagHook = safetyhook::create_mid(sprintStopPattern.get_first(), [](SafetyHookContext& regs)
                {
                    Diag(std::format("run ended by action {} pad={}",
                                     DiagActionName(static_cast<uint32_t>(regs.eax)), IsPadActiveDevice()));
                });
            }

            // 101447D2  CMP   EAX,[0x10F93A78]          ; sprint up
            // 101447D8  JNZ   0x101447F2
            // 101447DA  CALL  0x1007E1B0                <- hook
            // 101447DF  AND   byte ptr [EAX+0x4],0xBF
            // 101447E3  MOV   ECX,ESI
            // 101447E5  CALL  0x10143650                ; end the run
            // 101447EA  POP   EDI                       <- join
            auto sprintReleasePattern = dunia_pattern("75 18 E8 ? ? ? ? 80 60 04 BF 8B CE E8 ? ? ? ? 5F 5E 83 C4 10 C2 08 00");
            if (!sprintReleasePattern.empty())
            {
                static auto nSprintReleaseJoin = reinterpret_cast<uintptr_t>(sprintReleasePattern.get_first(18));

                static auto SprintReleaseHook = safetyhook::create_mid(sprintReleasePattern.get_first(2), [](SafetyHookContext& regs)
                {
                    // Only latch a run that is actually happening. Tapping while standing still
                    // would otherwise arm a sprint that starts on the next step forward and never
                    // ends, which is the permanent sprint this is meant to avoid.
                    auto nBlock = StateBlock(regs.esi);
                    auto bSprinting = nBlock != 0
                        && (*(uint8_t*)(nBlock + nCurrentState + nStateFlags) & nSprintFlag) != 0;

                    auto bSuppress = OnRelease(SprintState, IsPadActiveDevice() ? bSprintToggleController : bSprintToggle,
                                               bSprinting);

                    Diag(std::format("sprint up action, sprinting={} suppressed={} pad={}",
                                     bSprinting, bSuppress, IsPadActiveDevice()));

                    if (bSuppress)
                        regs.eip = nSprintReleaseJoin;
                });
            }

            // Once a frame the controller checks whether each action still exists in the action map
            // currently on the stack, and drops the matching state when it does not - that is how
            // the sights come down on entering a vehicle or a menu. This is not the hold mechanism,
            // so nothing here is suppressed; it is only used as a per-frame place to notice that the
            // engine has ended the state on its own and give the button back to the player.
            //
            // 10143B40  TEST  AL,AL                     <- hook, AL = action is bound in this map
            // 10143B42  JNZ   0x10143B50
            // 10143B44  MOV   ECX,[ESI+0x20]
            // 10143B47  CALL  0x1007E1B0
            // 10143B4C  AND   byte ptr [EAX+0x4],0xF7
            auto aimScopePattern = dunia_pattern("84 C0 75 0C 8B 4E 20 E8 ? ? ? ? 80 60 04 F7");
            if (!aimScopePattern.empty())
            {
                static auto AimScopeHook = safetyhook::create_mid(aimScopePattern.get_first(), [](SafetyHookContext& regs)
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

            auto sprintScopePattern = dunia_pattern("84 C0 75 13 8B 4E 20 E8 ? ? ? ? 80 60 04 BF 8B CE");
            if (!sprintScopePattern.empty())
            {
                static auto SprintScopeHook = safetyhook::create_mid(sprintScopePattern.get_first(), [](SafetyHookContext& regs)
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
