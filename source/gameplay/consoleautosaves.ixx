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
static constexpr uintptr_t nPopupSection = 0x04;
static constexpr uintptr_t nPopupText = 0x0C;
static constexpr uintptr_t nPopupTime = 0x18;

static constexpr char szPopupSectionMission[] = "Mission";

// The two keys that end a mission; library missions end on OBJECTIVE_COMPLETED_SBV and never
// raise MISSION_CONCLUDED.
static constexpr const char* PopupMissionEndKeys[] =
{
    "MISSION_CONCLUDED",
    "OBJECTIVE_COMPLETED_SBV",
};

// COnScreenPopup replaying one objective's entries, [esp+4] a std::string* objective name; runs
// immediately before the PushNewObjective calls it makes.
static constexpr uint64_t nObjectiveNameWindowMs = 100;

// Shared conclusion objective of every underground mission, tutorial handoff included; only a
// finished mission raises CompletedGrinMissions, written after the push in the same frame.
static constexpr char szObjectiveUnderground[] = "A0GM00_00";
static constexpr uint64_t nGrinWindowMs = 5000;

// CFCXMissionManager::CompletedGrinMissions, a byte.
static constexpr uintptr_t nMissionMgrGrinCount = 0x128;

// CPlayerSoundAndFXComponent: +0x4FC a pending save point check request, +0x500 its cooldown.
static constexpr uintptr_t nPlayerFXSavePointRequest = 0x4FC;
static constexpr uintptr_t nPlayerFXSavePointCooldown = 0x500;

// A tick gap above this is a load, not gameplay.
static constexpr uint64_t nGameplayGapMs = 1000;

// Level init replays popups, so a banner is only believed after this much gameplay.
static constexpr uint64_t nGameplaySettleMs = 3000;

// Box lands under the banner, not on its frame. A floor; the engine's busy gate can delay more.
static constexpr uint64_t nPromptDelayMs = 2000;
static constexpr uint64_t nPromptDelayMinMs = 500;

// Popups carry 5.0f (reputation poll) or 2.0f (OnScreenData.xml); outside this is not a duration.
static constexpr float fPopupTimeMin = 0.5f;
static constexpr float fPopupTimeMax = 30.0f;

// Zero means nothing pending; otherwise the timestamp the delay runs from.
static uint64_t nArmedAt = 0;
static uint64_t nArmedDelay = nPromptDelayMs;

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
static constexpr int32_t nGrinUnsampled = -1;
static int32_t nGrinSeen = nGrinUnsampled;
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
    if (!pMgr || IsBadReadPtr((void*)pMgr, nMissionMgrGrinCount + 1))
        return 0;

    return pMgr;
}

// Negative for an unreadable manager, not the same as a count of zero.
static int32_t GrinCount()
{
    auto pMgr = MissionMgr();
    return pMgr ? *(uint8_t*)(pMgr + nMissionMgrGrinCount) : nGrinUnsampled;
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

    if (nLastTick == 0 || (nNow - nGameplaySince) < nGameplaySettleMs || nArmedAt)
        return;

    nArmedAt = nNow;
    nArmedDelay = nDelay;
    bArmedNeedsGrin = bNeedsGrin;
    nArmedGrinFloor = nNow - nGrinWindowMs;
}

// Offsets into the mission manager lookup at the head of the prologue it is read from.
static constexpr ptrdiff_t nLookupRegisteredFlag = 5;
static constexpr ptrdiff_t nLookupRegistry = 15;
static constexpr ptrdiff_t nLookupDescriptor = 33;
static constexpr ptrdiff_t nLookupGetComponentCall = 39;

static SafetyHookMid ObjectivePopupHook{};
static SafetyHookMid PushNewObjectiveHook{};
static SafetyHookMid SavePointRequestHook{};

static void ReadMissionMgrLookup(uint8_t* pPrologue)
{
    pRegisteredFlag = *(uintptr_t*)(pPrologue + nLookupRegisteredFlag);
    pRegistry = *(uintptr_t*)(pPrologue + nLookupRegistry);
    pMgrDescriptor = *(uintptr_t*)(pPrologue + nLookupDescriptor);
    pGetComponent = reinterpret_cast<GetComponentFn>(dunia_call_target(pPrologue + nLookupGetComponentCall));
}

static bool IsMissionEndKey(const char* pText)
{
    for (auto pKey : PopupMissionEndKeys)
    {
        if (_stricmp(pText, pKey) == 0)
            return true;
    }

    return false;
}

// Half the banner on the shorter popups, so the box still lands under it.
static uint64_t PromptDelay(float fTime)
{
    if (!(fTime >= fPopupTimeMin && fTime <= fPopupTimeMax))
        return nPromptDelayMs;

    auto nOnScreen = static_cast<uint64_t>(fTime * 1000.0f);
    if (nOnScreen >= nPromptDelayMs + nPromptDelayMinMs)
        return nPromptDelayMs;

    auto nHalf = nOnScreen / 2;
    return nHalf > nPromptDelayMinMs ? nHalf : nPromptDelayMinMs;
}

static void OnObjectivePopup(SafetyHookContext& regs)
{
    auto pName = *(uintptr_t*)(static_cast<uintptr_t>(regs.esp) + 4);

    ReadDuniaString(pName, szObjective, sizeof(szObjective));
    nObjectiveAt = GetTickCount64();
}

static void OnPushNewObjective(SafetyHookContext& regs)
{
    auto nStack = static_cast<uintptr_t>(regs.esp);

    auto pSection = SafeString(*(uintptr_t*)(nStack + nPopupSection));
    auto pText = SafeString(*(uintptr_t*)(nStack + nPopupText));
    if (!pSection || !pText || _stricmp(pSection, szPopupSectionMission) != 0)
        return;

    if (!IsMissionEndKey(pText))
        return;

    auto nNow = GetTickCount64();

    auto bUnderground = (nNow - nObjectiveAt) <= nObjectiveNameWindowMs
        && _stricmp(szObjective, szObjectiveUnderground) == 0;

    auto nGrin = GrinCount();
    auto bGrinRose = (nGrin >= 0 && nGrinSeen >= 0 && nGrin > nGrinSeen)
        || (nGrinRoseAt && (nNow - nGrinRoseAt) <= nGrinWindowMs);

    auto fTime = *(float*)(nStack + nPopupTime);
    Arm(PromptDelay(fTime), bUnderground && !bGrinRose);
}

// A new player component is a new session; a long gap between ticks is a load.
static void UpdateSession(uintptr_t pPlayerFX, uint64_t nNow)
{
    auto nGap = nLastTick ? (nNow - nLastTick) : 0;

    if (pPlayerFX != pPlayerFXSeen)
    {
        pPlayerFXSeen = pPlayerFX;
        nGameplaySince = nNow;

        // A restored board is not progress the player just made.
        nGrinSeen = nGrinUnsampled;
        nGrinRoseAt = 0;

        nArmedAt = 0;
    }
    else if (nGap > nGameplayGapMs && nArmedAt)
    {
        // Restarted from the first running tick; a fade would spend it on black.
        nArmedAt = nNow;
    }

    nLastTick = nNow;
}

static void UpdateGrin(uint64_t nNow)
{
    auto nGrin = GrinCount();
    if (nGrin < 0)
        return;

    if (nGrinSeen >= 0 && nGrin > nGrinSeen)
        nGrinRoseAt = nNow;

    nGrinSeen = nGrin;
}

// Clears the armed prompt once it is due; true when the save should still go ahead.
static bool TakeArmedSave(uint64_t nNow)
{
    if (!nArmedAt)
        return false;

    // Checked here so switching the ini off mid-mission drops a pending request.
    if (!bConsoleAutosaves)
    {
        nArmedAt = 0;
        return false;
    }

    if ((nNow - nArmedAt) < nArmedDelay)
        return false;

    nArmedAt = 0;

    // The tutorial handoff raises the underground banner and records nothing.
    return !(bArmedNeedsGrin && nGrinRoseAt < nArmedGrinFloor);
}

// EDI is the CPlayerSoundAndFXComponent. Also the module's clock.
static void OnSavePointRequest(SafetyHookContext& regs)
{
    auto pPlayerFX = static_cast<uintptr_t>(regs.edi);
    if (!pPlayerFX)
        return;

    auto nNow = GetTickCount64();
    UpdateSession(pPlayerFX, nNow);
    UpdateGrin(nNow);

    if (!TakeArmedSave(nNow))
        return;

    *(float*)(pPlayerFX + nPlayerFXSavePointCooldown) = 0.0f;
    *(uint8_t*)(pPlayerFX + nPlayerFXSavePointRequest) = 1;
}

class ConsoleAutosaves
{
public:
    ConsoleAutosaves()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            BindBool(bConsoleAutosaves, PREF_CONSOLEAUTOSAVES);

            // Not hooked; the prologue is only where the manager lookup above is read from.
            if (auto* p = dunia_find("83 EC 20 83 3D ? ? ? ? 00 53 56 57 8B 3D ? ? ? ? 8B F1 75 07"))
                ReadMissionMgrLookup(static_cast<uint8_t*>(p));

            // COnScreenPopup walking one objective's popup list; only the name is taken here.
            if (auto* p = dunia_find("83 EC 48 8B 44 24 4C 53 55 56 57 8B F9 50 8D 4C 24 1C 51 8D 4F 04"))
                ObjectivePopupHook = safetyhook::create_mid(p, OnObjectivePopup);

            // CFCXObjectiveHudManager::PushNewObjective, anchored on the prologue.
            if (auto* p = dunia_find("83 EC 58 53 55 56 57 8D 44 24 13 8B F9 50 8D 4C 24 54 33 DB"))
                PushNewObjectiveHook = safetyhook::create_mid(p, OnPushNewObjective);

            // Fired through CPlayerSoundAndFXComponent::Update's own request slot, not a raw
            // dispatcher post that would land mid-cutscene. Hook on the CMP, so the flag written
            // here is seen when the relocated instruction re-executes.
            if (auto* p = dunia_find("80 BF FC 04 00 00 00 F3 0F 10 87 00 05 00 00 F3 0F 5C 44 24 60"))
                SavePointRequestHook = safetyhook::create_mid(p, OnSavePointRequest);
        };
    }
} ConsoleAutosaves;
