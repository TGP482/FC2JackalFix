module;

#include <common.hxx>
#include <string>

export module saveonmissioncomplete;

import common;
import dunia;
import settings;

static bool bConsoleAutosaves = false;

// CFCXObjectiveHudManager::PushNewObjective, __thiscall, six stack arguments and RET 0x18:
// [esp+4] oasis section, [esp+8] icon, [esp+0xC] text key, [esp+0x10] and [esp+0x14] unused here,
// [esp+0x18] display time in seconds. Both strings arrive as char*, not std::string.
static constexpr uintptr_t POPUP_SECTION = 0x04;
static constexpr uintptr_t POPUP_TEXT = 0x0C;
static constexpr uintptr_t POPUP_TIME = 0x18;

static constexpr char POPUP_SECTION_MISSION[] = "Mission";

// The two keys that end a mission. MISSION_CONCLUDED is "Mission Completed". Every library
// mission's buddy branch ends on OBJECTIVE_COMPLETED_SBV instead, "Safe House Upgraded", and never
// raises MISSION_CONCLUDED: OnScreenData.xml carries it on the last objective of all twelve
// (A1LM01_05 through A2LM12_05). OBJECTIVE_COMPLETED and OBJECTIVE_ABORTED are used by no entry.
static constexpr const char* PopupMissionEndKeys[] =
{
    "MISSION_CONCLUDED",
    "OBJECTIVE_COMPLETED_SBV",
};

// COnScreenPopup replaying one objective's entries (0x107098B0), __thiscall, [esp+4] a std::string*
// holding the objective name. It runs immediately before the PushNewObjective calls it makes.
static constexpr uint64_t OBJECTIVE_NAME_WINDOW_MS = 100;

// A0GM00_00 is the shared conclusion objective of every underground mission, the tutorial handoff
// included, and the handoff is the one banner the console does not save on. What separates them is
// CompletedGrinMissions, which only a finished mission raises. The counter is written after the
// push and inside the same frame, so the prompt is armed and the fire drops it if it never moved.
static constexpr char OBJECTIVE_UNDERGROUND[] = "A0GM00_00";
static constexpr uint64_t GRIN_WINDOW_MS = 5000;

// CFCXMissionManager, from the property registration in FUN_107567F0. CompletedGrinMissions is a
// byte.
static constexpr uintptr_t MISSIONMGR_GRINCOUNT = 0x128;

// CPlayerSoundAndFXComponent: +0x4FC a pending save point check request, +0x500 its cooldown.
static constexpr uintptr_t PLAYERFX_SAVEPOINTREQUEST = 0x4FC;
static constexpr uintptr_t PLAYERFX_SAVEPOINTCOOLDOWN = 0x500;

// A tick gap this long is not a load: over ~25 minutes, 27 gaps against three distinct player
// components.
static constexpr uint64_t GAMEPLAY_GAP_MS = 1000;

// Gameplay has to have been running this long before a banner is believed, because level init
// replays popups the player never sees.
static constexpr uint64_t GAMEPLAY_SETTLE_MS = 3000;

// Long enough that the box does not land on the same frame as the banner, short enough that the
// banner is still up under it. A floor: the engine's busy gate can hold the prompt back further.
static constexpr uint64_t PROMPT_DELAY_MS = 2000;
static constexpr uint64_t PROMPT_DELAY_MIN_MS = 500;

// Popups carry 5.0f from the reputation poll and 2.0f from OnScreenData.xml's time attribute.
// Anything outside this range is not a duration and the default delay stands.
static constexpr float POPUP_TIME_MIN = 0.5f;
static constexpr float POPUP_TIME_MAX = 30.0f;

// Zero means nothing pending; otherwise the timestamp the delay runs from.
static uint64_t nArmedAt = 0;
static uint64_t nArmedDelay = PROMPT_DELAY_MS;

// The earliest counter rise that still belongs to the armed banner. nArmedAt itself moves on a
// stall, so the floor is kept separately.
static bool bArmedNeedsGrin = false;
static uint64_t nArmedGrinFloor = 0;

static uint64_t nLastTick = 0;
static uint64_t nGameplaySince = 0;

// The component is rebuilt with the player, so a change here is a session change and a gap in the
// clock is not.
static uintptr_t pPlayerFXSeen = 0;

static char szObjective[64] = {};
static uint64_t nObjectiveAt = 0;

// Sampled per tick rather than per banner: the counter moves in the same frame as the push, and a
// sample taken at the banner would take the rise as its own baseline.
static constexpr int32_t GRIN_UNSAMPLED = -1;
static int32_t nGrinSeen = GRIN_UNSAMPLED;
static uint64_t nGrinRoseAt = 0;

// CFCXMissionManager is a registered game component, so the instance comes out of the lookup the
// engine uses. FUN_10747FC0's prologue carries every piece of it:
//
//   10747FC3  CMP    dword ptr [11649EAC], 0   ; registered yet
//   10747FCD  MOV    EDI, [11644D74]           ; the component registry
//   10747FE0  PUSH   0x11649EBC                ; the manager's descriptor
//   10747FE7  CALL   0x104DBB80                ; __thiscall GetComponent(descriptor, 1)
//
// FUN_10666680 registers both globals under the literal CFCXMissionManager. All four operands are
// relocated at load, so they are read back out of the instructions rather than rebased by hand.
// Waiting on one of the manager's own methods for a this pointer is not enough: the second
// underground mission of a run can end before SelectMission or GiveMissionReward has run once.
using GetComponentFn = uintptr_t(__thiscall*)(uintptr_t, uintptr_t, int);
static GetComponentFn pGetComponent = nullptr;
static uintptr_t pRegisteredFlag = 0;
static uintptr_t pRegistry = 0;
static uintptr_t pMgrDescriptor = 0;

static uintptr_t MissionMgr()
{
    if (!pGetComponent || !pRegisteredFlag || !*(int32_t*)pRegisteredFlag)
        return 0;

    auto pRegistryObject = *(uintptr_t*)pRegistry;
    if (!pRegistryObject)
        return 0;

    auto pMgr = pGetComponent(pRegistryObject, pMgrDescriptor, 1);
    if (!pMgr || IsBadReadPtr((void*)pMgr, MISSIONMGR_GRINCOUNT + 1))
        return 0;

    return pMgr;
}

// Negative for an unreadable manager, which is not the same as a count of zero.
static int32_t GrinCount()
{
    auto pMgr = MissionMgr();
    return pMgr ? *(uint8_t*)(pMgr + MISSIONMGR_GRINCOUNT) : GRIN_UNSAMPLED;
}

static const char* SafeString(uintptr_t p)
{
    if (!p || IsBadStringPtrA((LPCSTR)p, 128))
        return nullptr;
    return (const char*)p;
}

// One banner is one prompt, and the delay is measured from the banner. A banner pushed across a
// stall is still a banner; the tick hook moves the delay for it.
static void Arm(uint64_t nDelay, bool bNeedsGrin)
{
    auto nNow = GetTickCount64();

    if (nLastTick == 0 || (nNow - nGameplaySince) < GAMEPLAY_SETTLE_MS || nArmedAt)
        return;

    nArmedAt = nNow;
    nArmedDelay = nDelay;
    bArmedNeedsGrin = bNeedsGrin;
    nArmedGrinFloor = nNow - GRIN_WINDOW_MS;
}

class SaveOnMissionComplete
{
public:
    SaveOnMissionComplete()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            BindBool(bConsoleAutosaves, PREF_CONSOLEAUTOSAVES);

            // 0x10747FC0, a buddy availability test. Nothing is hooked here; its prologue is only
            // where the manager lookup above is read from.
            if (auto* p = dunia_find("83 EC 20 83 3D ? ? ? ? 00 53 56 57 8B 3D ? ? ? ? 8B F1 75 07"))
            {
                auto n = reinterpret_cast<uintptr_t>(p);

                pRegisteredFlag = *(uintptr_t*)(n + 5);
                pRegistry = *(uintptr_t*)(n + 15);
                pMgrDescriptor = *(uintptr_t*)(n + 33);
                pGetComponent = reinterpret_cast<GetComponentFn>(n + 44 + *(int32_t*)(n + 40));
            }

            // 0x107098B0, COnScreenPopup walking one objective's popup list. Only the name is taken
            // here; PushNewObjective below is what arms.
            if (auto* p = dunia_find("83 EC 48 8B 44 24 4C 53 55 56 57 8B F9 50 8D 4C 24 1C 51 8D 4F 04"))
            {
                static auto ObjectivePopupHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto pName = *(uintptr_t*)(static_cast<uintptr_t>(regs.esp) + 4);

                    ReadDuniaString(pName, szObjective, sizeof(szObjective));
                    nObjectiveAt = GetTickCount64();
                });
            }

            // 0x10686080, CFCXObjectiveHudManager::PushNewObjective. Anchored on the prologue; the
            // oasis lookup further in is shared with other pages.
            if (auto* p = dunia_find("83 EC 58 53 55 56 57 8D 44 24 13 8B F9 50 8D 4C 24 54 33 DB"))
            {
                static auto PushNewObjectiveHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto nStack = static_cast<uintptr_t>(regs.esp);

                    auto pSection = SafeString(*(uintptr_t*)(nStack + POPUP_SECTION));
                    auto pText = SafeString(*(uintptr_t*)(nStack + POPUP_TEXT));
                    if (!pSection || !pText || _stricmp(pSection, POPUP_SECTION_MISSION) != 0)
                        return;

                    auto bMissionEnd = false;
                    for (auto pKey : PopupMissionEndKeys)
                        bMissionEnd = bMissionEnd || _stricmp(pText, pKey) == 0;

                    if (!bMissionEnd)
                        return;

                    auto nNow = GetTickCount64();

                    auto bUnderground = (nNow - nObjectiveAt) <= OBJECTIVE_NAME_WINDOW_MS
                        && _stricmp(szObjective, OBJECTIVE_UNDERGROUND) == 0;

                    auto nGrin = GrinCount();
                    auto bGrinRose = (nGrin >= 0 && nGrinSeen >= 0 && nGrin > nGrinSeen)
                        || (nGrinRoseAt && (nNow - nGrinRoseAt) <= GRIN_WINDOW_MS);

                    auto nDelay = PROMPT_DELAY_MS;

                    // Half the banner on the shorter popups, so the box still lands under it.
                    auto fTime = *(float*)(nStack + POPUP_TIME);
                    if (fTime >= POPUP_TIME_MIN && fTime <= POPUP_TIME_MAX)
                    {
                        auto nOnScreen = static_cast<uint64_t>(fTime * 1000.0f);
                        if (nOnScreen < PROMPT_DELAY_MS + PROMPT_DELAY_MIN_MS)
                            nDelay = (nOnScreen / 2 > PROMPT_DELAY_MIN_MS) ? (nOnScreen / 2)
                                                                           : PROMPT_DELAY_MIN_MS;
                    }

                    Arm(nDelay, bUnderground && !bGrinRose);
                });
            }

            //
            // ---- Firing, through the game's own deferred request ----
            //
            // A raw post through the dispatcher at 0x104DBA30 would land mid-cutscene or mid-fade.
            // CPlayerSoundAndFXComponent::Update (0x106D2C20) carries a request slot instead:
            //
            //   106D2F42  CMP    byte ptr [EDI+0x4FC], 0     ; request pending?
            //   106D2F51  SUBSS  XMM0, [ESP+0x60]            ; cooldown [EDI+0x500] -= dt
            //   106D2F5F  JZ     0x106D3135                  ; no request, done
            //   106D2F6C  JBE    0x106D2F75                  ; cooldown expired, let it through
            //   106D2F6E  MOV    byte ptr [EDI+0x4FC], 0     ; still cooling down, drop it
            //             ...three gates, then post GREventSavePointCheck, cooldown = 5.0f
            //
            // The third gate (DAT_11649EF4 vtable +0x14 at 0x106D2FCE) is a busy test; failing it
            // jumps to the tail without clearing the flag, so the component retries next tick.
            //
            // The hook sits on the CMP, so the flag written here is seen when the relocated
            // instruction re-executes; zeroing the cooldown first stops 0x106D2F6E dropping it.
            // Also the module's clock, and where the grin counter is sampled.
            if (auto* p = dunia_find("80 BF FC 04 00 00 00 F3 0F 10 87 00 05 00 00 F3 0F 5C 44 24 60"))
            {
                static auto SavePointRequestHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto pPlayerFX = static_cast<uintptr_t>(regs.edi);
                    if (!pPlayerFX)
                        return;

                    auto nNow = GetTickCount64();
                    auto nGap = nLastTick ? (nNow - nLastTick) : 0;

                    if (pPlayerFX != pPlayerFXSeen)
                    {
                        pPlayerFXSeen = pPlayerFX;
                        nGameplaySince = nNow;

                        // A restored board is not progress the player just made.
                        nGrinSeen = GRIN_UNSAMPLED;
                        nGrinRoseAt = 0;

                        nArmedAt = 0;
                    }
                    else if (nGap > GAMEPLAY_GAP_MS && nArmedAt)
                    {
                        // A banner pushed under a fade spends its delay on a black screen, so the
                        // delay restarts from the first running tick instead of crediting the gap.
                        nArmedAt = nNow;
                    }
                    nLastTick = nNow;

                    auto nGrin = GrinCount();
                    if (nGrin >= 0)
                    {
                        if (nGrinSeen >= 0 && nGrin > nGrinSeen)
                            nGrinRoseAt = nNow;

                        nGrinSeen = nGrin;
                    }

                    if (!nArmedAt)
                        return;

                    // Checked here so switching the ini off mid-mission drops a pending request.
                    if (!bConsoleAutosaves)
                    {
                        nArmedAt = 0;
                        return;
                    }

                    if ((nNow - nArmedAt) < nArmedDelay)
                        return;

                    nArmedAt = 0;

                    // The tutorial handoff raises the underground banner and records nothing.
                    if (bArmedNeedsGrin && nGrinRoseAt < nArmedGrinFloor)
                        return;

                    *(float*)(pPlayerFX + PLAYERFX_SAVEPOINTCOOLDOWN) = 0.0f;
                    *(uint8_t*)(pPlayerFX + PLAYERFX_SAVEPOINTREQUEST) = 1;
                });
            }
        };
    }
} SaveOnMissionComplete;
