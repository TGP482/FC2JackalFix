module;

#include <common.hxx>
#include <string>

export module saveonmissioncomplete;

import common;
import dunia;
import settings;

static bool bConsoleAutosaves = false;

// CFCXObjectiveHudManager::PushNewObjective, __thiscall, RET 0x18: [esp+4] oasis section,
// [esp+0xC] text key, [esp+0x18] display seconds. Both strings are char*, not std::string.
static constexpr uintptr_t POPUP_SECTION = 0x04;
static constexpr uintptr_t POPUP_TEXT = 0x0C;
static constexpr uintptr_t POPUP_TIME = 0x18;

static constexpr char POPUP_SECTION_MISSION[] = "Mission";

// The two keys that end a mission; library missions end on OBJECTIVE_COMPLETED_SBV and never
// raise MISSION_CONCLUDED.
static constexpr const char* PopupMissionEndKeys[] =
{
    "MISSION_CONCLUDED",
    "OBJECTIVE_COMPLETED_SBV",
};

// COnScreenPopup replaying one objective's entries, [esp+4] a std::string* objective name; runs
// immediately before the PushNewObjective calls it makes.
static constexpr uint64_t OBJECTIVE_NAME_WINDOW_MS = 100;

// Shared conclusion objective of every underground mission, tutorial handoff included; only a
// finished mission raises CompletedGrinMissions, written after the push in the same frame.
static constexpr char OBJECTIVE_UNDERGROUND[] = "A0GM00_00";
static constexpr uint64_t GRIN_WINDOW_MS = 5000;

// CFCXMissionManager::CompletedGrinMissions, a byte.
static constexpr uintptr_t MISSIONMGR_GRINCOUNT = 0x128;

// CPlayerSoundAndFXComponent: +0x4FC a pending save point check request, +0x500 its cooldown.
static constexpr uintptr_t PLAYERFX_SAVEPOINTREQUEST = 0x4FC;
static constexpr uintptr_t PLAYERFX_SAVEPOINTCOOLDOWN = 0x500;

// A tick gap above this is a load, not gameplay.
static constexpr uint64_t GAMEPLAY_GAP_MS = 1000;

// Level init replays popups, so a banner is only believed after this much gameplay.
static constexpr uint64_t GAMEPLAY_SETTLE_MS = 3000;

// Box lands under the banner, not on its frame. A floor; the engine's busy gate can delay more.
static constexpr uint64_t PROMPT_DELAY_MS = 2000;
static constexpr uint64_t PROMPT_DELAY_MIN_MS = 500;

// Popups carry 5.0f (reputation poll) or 2.0f (OnScreenData.xml); outside this is not a duration.
static constexpr float POPUP_TIME_MIN = 0.5f;
static constexpr float POPUP_TIME_MAX = 30.0f;

// Zero means nothing pending; otherwise the timestamp the delay runs from.
static uint64_t nArmedAt = 0;
static uint64_t nArmedDelay = PROMPT_DELAY_MS;

// Earliest counter rise still belonging to the armed banner; nArmedAt moves on a stall.
static bool bArmedNeedsGrin = false;
static uint64_t nArmedGrinFloor = 0;

static uint64_t nLastTick = 0;
static uint64_t nGameplaySince = 0;

// Rebuilt with the player, so a change here is a session change and a clock gap is not.
static uintptr_t pPlayerFXSeen = 0;

static char szObjective[64] = {};
static uint64_t nObjectiveAt = 0;

// Sampled per tick, not per banner: the counter moves in the same frame as the push.
static constexpr int32_t GRIN_UNSAMPLED = -1;
static int32_t nGrinSeen = GRIN_UNSAMPLED;
static uint64_t nGrinRoseAt = 0;

// The instance comes from the component registry, all four operands read back out of the prologue
// below since they are relocated. Waiting on one of the manager's own methods is not enough: a
// run's second underground mission can end before any of them has run.
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

// Negative for an unreadable manager, not the same as a count of zero.
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

// One banner, one prompt, delay measured from the banner; the tick hook moves it across a stall.
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

            // Not hooked; the prologue is only where the manager lookup above is read from.
            if (auto* p = dunia_find("83 EC 20 83 3D ? ? ? ? 00 53 56 57 8B 3D ? ? ? ? 8B F1 75 07"))
            {
                auto n = reinterpret_cast<uintptr_t>(p);

                pRegisteredFlag = *(uintptr_t*)(n + 5);
                pRegistry = *(uintptr_t*)(n + 15);
                pMgrDescriptor = *(uintptr_t*)(n + 33);
                pGetComponent = reinterpret_cast<GetComponentFn>(n + 44 + *(int32_t*)(n + 40));
            }

            // COnScreenPopup walking one objective's popup list; only the name is taken here.
            if (auto* p = dunia_find("83 EC 48 8B 44 24 4C 53 55 56 57 8B F9 50 8D 4C 24 1C 51 8D 4F 04"))
            {
                static auto ObjectivePopupHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto pName = *(uintptr_t*)(static_cast<uintptr_t>(regs.esp) + 4);

                    ReadDuniaString(pName, szObjective, sizeof(szObjective));
                    nObjectiveAt = GetTickCount64();
                });
            }

            // CFCXObjectiveHudManager::PushNewObjective, anchored on the prologue.
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

            // Fired through CPlayerSoundAndFXComponent::Update's own request slot, not a raw
            // dispatcher post that would land mid-cutscene. Hook on the CMP, so the flag written
            // here is seen when the relocated instruction re-executes. Also the module's clock.
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
                        // Restarted from the first running tick; a fade would spend it on black.
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
