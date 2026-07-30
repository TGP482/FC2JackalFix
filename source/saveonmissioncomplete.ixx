/*
  The 360 build popped a "Save Game" box on mission completion. Everything behind it is intact on
  PC. Things do post the event; none of them is a mission ending.

    GREventSavePointCheck -> CFCXGRStateSingleInGame::OnGREvent (0x107F8770), sets state+0x68
                          -> CFCXGRStateSavePointCheck::Enter   (0x107F5C70)
                          -> CSavePointCheckPage::OnEnter       (0x10850A90)

  The page builds exactly the 360's box: header MBOXHEADER_SAVEGAME, entries MBOXLISTSAVE_SAVEGAME
  and MBOXLISTSAVE_CONTINUENOSAVE out of the MessagesBoxListSavePointCheck table. Its selection
  callback (0x108509A0) either pushes CSavePointSaveGamePage or fires GREventNextState. Neither page
  nor state tests for a save point, so all gating lives in whatever posts the event. The name string
  (0x10E95B34) is materialised at seven sites:

    CBedroll tick (safehouse bed)         0x106C184F
    CBedroll::OnMessage, "Save" branch    0x106C1DA3
    CPlayerSoundAndFXComponent::Update    0x106D3027, 0x106D3036   <- the slot this module drives
    SaveGame script command               0x1070E31C, 0x1070E32B
    OnGREvent consumer                    0x107F8958

  So the 360's behaviour came from its mission scripts calling SaveGame() in the packed data. Do not
  re-check the 360 executable: the string is at 0x820754DC, but nothing in the disassembled portion
  materialises it with a lis/addi pair, so its call sites cannot be enumerated the same way.

  ---- What counts as a mission completing ----

  CFCXMissionManager::MissionCompleted (0x107532A0), in test order:

    name == "SAVE_BUDDY" / "RESCUE_REUBEN"                    reputation +5,  records nothing
    name == "HOUSE_CLEANING1_BASE" / "..2_BASE"               reputation +10, records nothing
    name == "HOUSE_CLEANING1_SUBVERT" / "..2_SUBVERT"         reputation +20, records nothing
    name == "BUDDY_BETRAYAL" / "KILL_WARLORDS"                reputation +20, records nothing
    name found in BSQMissions   (+0x130)                      entry+0x40 = 1
    name == "GRIN"                                            CompletedGrinMissions (+0x128)++
    name[2..3] == "CV"                                        CompletedConvoyMissions (+0x12A)++
    name[2..3] == "AS"                                        CompletedAssassinationMissions (+0x12C)++
    selType == Story   (2)                                    StoryMission   entry+0x40 = 1
    selType == Library (3)                                    LibraryMission entry+0x40 = 1
    selType == Buddy   (4)                                    BuddyMission   entry+0x40 = 1

  The first eight rows all return early, so buddy sidequests, GRIN, convoys and assassinations never
  reach the selType branches, never call SetCurrentMissionInfo(None), and never refresh the board.

  Route 1 polls the record instead. The three byte counters and four flag arrays above are, between
  them, everything the manager stores about missions the player has finished, so sampling them once
  a frame catches every mission type by construction. The function-shaped routes latch into the same
  slot and cover the window before the manager pointer is known.
*/

module;

#include <common.hxx>
#include <iterator>
#include <vector>

export module saveonmissioncomplete;

import common;
import dunia;
import settings;

static bool bConsoleAutosaves = false;

//
// ---- CFCXMissionManager ----
//
// Layout from the property registration in FUN_107567F0 and the reset in FUN_10753060.
//

// CurrentMissionInfo: selType an int32 enum, Index a byte. The enum's members are registered in
// order, so the SEL_ values below are read off the registration rather than guessed.
static constexpr uintptr_t MISSIONMGR_SELTYPE = 0x174;
static constexpr uintptr_t MISSIONMGR_INDEX = 0x178;
static constexpr uintptr_t MISSIONMGR_REPUTATION = 0x180;

static constexpr int32_t SEL_NONE = 0;
static constexpr int32_t SEL_LIBRARYOFFERED = 1;   // a list of offers, no single mission
static constexpr int32_t SEL_STORY = 2;
static constexpr int32_t SEL_LIBRARY = 3;
static constexpr int32_t SEL_BUDDY = 4;

// CFCXMission (ctor 0x1074EC40) bases all four arrays' entries; they only differ below +0x40.
static constexpr uintptr_t MISSION_NAME = 0x08;
static constexpr uintptr_t MISSION_COMPLETED = 0x40;

struct MissionArrayDef
{
    uintptr_t nBase;
    uintptr_t nCount;   // int32
    size_t nStride;
    const char* pWhat;
};

static constexpr MissionArrayDef MissionArrays[] =
{
    { 0x44,  0x48,  0x50, "story"           },
    { 0x5C,  0x60,  0x54, "library"         },
    { 0x88,  0x8C,  0x50, "buddy"           },
    { 0x130, 0x134, 0x48, "buddy sidequest" },
};
static constexpr size_t MISSION_ARRAYS = std::size(MissionArrays);

struct MissionCounterDef
{
    uintptr_t nOffset;
    const char* pWhat;
};

// These three are tallied instead of flagged, because they have no board entry to flag.
static constexpr MissionCounterDef MissionCounters[] =
{
    { 0x128, "bonus"         },   // CompletedGrinMissions
    { 0x12A, "convoy"        },   // CompletedConvoyMissions
    { 0x12C, "assassination" },   // CompletedAssassinationMissions
};
static constexpr size_t MISSION_COUNTERS = std::size(MissionCounters);

// A larger count means the array is not deserialised yet, or the pointer is stale. Retail's largest
// array is well under a hundred entries, so 512 is a safe sentinel.
static constexpr int32_t MAX_MISSION_ENTRIES = 512;

// Every early return above MissionCompleted's first recording branch. Complete as of 0x107532A0.
static const char* const ReputationOnlyNames[] =
{
    "SAVE_BUDDY",
    "RESCUE_REUBEN",
    "HOUSE_CLEANING1_BASE",
    "HOUSE_CLEANING2_BASE",
    "HOUSE_CLEANING1_SUBVERT",
    "HOUSE_CLEANING2_SUBVERT",
    "BUDDY_BETRAYAL",
    "KILL_WARLORDS",
};

// CPlayerSoundAndFXComponent: +0x4FC a pending save point check request, +0x500 its cooldown.
static constexpr uintptr_t PLAYERFX_SAVEPOINTREQUEST = 0x4FC;
static constexpr uintptr_t PLAYERFX_SAVEPOINTCOOLDOWN = 0x500;

// A tick gap this long is not a load: over ~25 minutes, 27 gaps against three distinct player
// components. It does mean the wall clock ran on, so the prompt delay is pushed out to match.
static constexpr uint64_t GAMEPLAY_GAP_MS = 1000;

// Gameplay has to have been running this long before a recorded completion is believed, because
// the manager is reset and deserialised at the start of a session and both look like one.
static constexpr uint64_t GAMEPLAY_SETTLE_MS = 3000;

// Several in one sample is the board being restored. Two because a script can close out a mission
// and its follow-up in the same frame.
static constexpr size_t MAX_COMPLETIONS_PER_SAMPLE = 2;

// How far apart a CGameMission going Started -> Ended and the board re-evaluating may be.
static constexpr uint64_t MISSION_END_WINDOW_MS = 2000;

// Otherwise the box lands on the same frame as the MISSION COMPLETE banner. This is a floor, and
// the engine's busy gate can still hold the prompt back further.
static constexpr uint64_t PROMPT_DELAY_MS = 2000;

// Zero means nothing pending; otherwise the timestamp the delay runs from.
static uint64_t nArmedAt = 0;
static uint64_t nMissionEndedAt = 0;

static uint64_t nLastTick = 0;
static uint64_t nGameplaySince = 0;

// The component is rebuilt with the player, so a change here is a session change and a gap in the
// clock is not.
static uintptr_t pPlayerFXSeen = 0;

// CBaseMission::SetState (0x10584D40, vtable slot +0x24) is a jump table over 0..8 with the odd
// values falling through to a no-op, so the state is a bitmask. The Lua Enable and Disable thunks
// (0x10585320, 0x10585340) name 1 and 2; 4 and 8 have no names in the DLL and are read off their
// order.
static constexpr int32_t MISSIONSTATE_NONE = 0;
static constexpr int32_t MISSIONSTATE_STARTED = 1;   // confirmed: Lua Enable
static constexpr int32_t MISSIONSTATE_ENDED = 2;     // confirmed: Lua Disable
static constexpr int32_t MISSIONSTATE_SUCCESS = 4;   // inferred
static constexpr int32_t MISSIONSTATE_FAILED = 8;    // inferred

static bool IsRealMission(int32_t nSelType)
{
    return nSelType == SEL_STORY || nSelType == SEL_LIBRARY || nSelType == SEL_BUDDY;
}

// MSVC8 std::string: proxy at +0, 16 byte SSO buffer at +4 which becomes a pointer once the
// capacity at +0x18 reaches 16, length at +0x14.
static const char* ReadDuniaString(uintptr_t pStr, char* pBuf, size_t nBufSize)
{
    pBuf[0] = '\0';
    if (!pStr || nBufSize < 2 || IsBadReadPtr((void*)pStr, 0x1C))
        return pBuf;

    auto nSize = *(uint32_t*)(pStr + 0x14);
    auto nCapacity = *(uint32_t*)(pStr + 0x18);
    if (nSize > 512 || nCapacity > 0x10000)
        return pBuf;

    auto pSrc = (nCapacity >= 16) ? *(const char**)(pStr + 4) : (const char*)(pStr + 4);
    if (!pSrc || IsBadReadPtr(pSrc, nSize))
        return pBuf;

    auto n = (nSize < nBufSize - 1) ? nSize : nBufSize - 1;
    for (size_t i = 0; i < n; i++)
        pBuf[i] = pSrc[i];
    pBuf[n] = '\0';
    return pBuf;
}

static int32_t SelType(uintptr_t pMissionMgr)
{
    return pMissionMgr ? *(int32_t*)(pMissionMgr + MISSIONMGR_SELTYPE) : -1;
}

// Every arming route funnels through here, so one completion still raises one prompt.
static void Arm()
{
    auto nNow = GetTickCount64();

    // Rejects hooks that ran while the game was stalled; they cannot know their session still exists.
    if (nLastTick == 0 || (nNow - nLastTick) >= GAMEPLAY_GAP_MS)
        return;

    if ((nNow - nGameplaySince) < GAMEPLAY_SETTLE_MS)
        return;

    // First arm wins; the delay is measured from the completion.
    if (nArmedAt)
        return;

    nArmedAt = nNow;
}

//
// ---- Route 1: the ledger ----
//
// The board's own record, sampled once a frame. Convoys, assassinations, sidequests and bonus
// missions never touch the state the other routes watch.
//

static uintptr_t pMissionMgr = 0;

static bool bLedgerValid = false;
static uint8_t nLedgerCounters[MISSION_COUNTERS] = {};
static std::vector<uint8_t> LedgerFlags[MISSION_ARRAYS];

// The first manager to turn up as a `this` lets the ledger start.
static void NoteMissionMgr(uintptr_t p)
{
    if (!p || p == pMissionMgr)
        return;

    // A different manager is a different session; nothing sampled off the old one is comparable.
    pMissionMgr = p;
    bLedgerValid = false;
}

static void InvalidateLedger()
{
    bLedgerValid = false;
}

// Static rather than local: four fresh vectors a frame would be four allocations a frame for the
// life of the process.
static std::vector<uint8_t> SampleFlags[MISSION_ARRAYS];

// Ranges are probed rather than trusted, because the base pointers are null through level changes.
static bool ReadMissionFlags(const MissionArrayDef& def, std::vector<uint8_t>& out)
{
    auto pBase = *(uintptr_t*)(pMissionMgr + def.nBase);
    auto nCount = *(int32_t*)(pMissionMgr + def.nCount);

    if (!pBase || nCount <= 0 || nCount > MAX_MISSION_ENTRIES)
        return false;
    if (IsBadReadPtr((void*)pBase, static_cast<UINT_PTR>(nCount) * def.nStride))
        return false;

    out.resize(static_cast<size_t>(nCount));
    for (int32_t i = 0; i < nCount; i++)
        out[static_cast<size_t>(i)] = *(uint8_t*)(pBase + static_cast<size_t>(i) * def.nStride + MISSION_COMPLETED) ? 1 : 0;

    return true;
}

// Arms on a newly recorded completion; re-baselines when the record looks rebuilt.
static void PollLedger()
{
    // Everything read below is inside the manager's first 0x184 bytes.
    if (!pMissionMgr || IsBadReadPtr((void*)pMissionMgr, MISSIONMGR_REPUTATION + sizeof(int32_t)))
        return;

    for (size_t a = 0; a < MISSION_ARRAYS; a++)
    {
        // Half a ledger would arm on the arrays that did read once the rest came back.
        if (!ReadMissionFlags(MissionArrays[a], SampleFlags[a]))
        {
            bLedgerValid = false;
            return;
        }
    }

    uint8_t counters[MISSION_COUNTERS];
    for (size_t c = 0; c < MISSION_COUNTERS; c++)
        counters[c] = *(uint8_t*)(pMissionMgr + MissionCounters[c].nOffset);

    if (!bLedgerValid)
    {
        for (size_t a = 0; a < MISSION_ARRAYS; a++)
            LedgerFlags[a] = SampleFlags[a];
        for (size_t c = 0; c < MISSION_COUNTERS; c++)
            nLedgerCounters[c] = counters[c];

        bLedgerValid = true;
        return;
    }

    // Counted first, acted on second: a restore onto a same-length board would otherwise arm on the
    // first newly-set flag before the count identified it as a restore.
    size_t nFound = 0;

    // A length change means the board was rebuilt, so the flags either side are different missions.
    for (size_t a = 0; a < MISSION_ARRAYS; a++)
    {
        auto& sample = SampleFlags[a];
        auto& known = LedgerFlags[a];

        if (known.size() != sample.size())
        {
            known = sample;
            continue;
        }

        for (size_t i = 0; i < sample.size(); i++)
        {
            if (sample[i] == known[i])
                continue;

            known[i] = sample[i];

            // Cleared rather than set is a reload, not a completion.
            if (sample[i])
                nFound++;
        }
    }

    for (size_t c = 0; c < MISSION_COUNTERS; c++)
    {
        if (counters[c] == nLedgerCounters[c])
            continue;

        if (counters[c] > nLedgerCounters[c])
            nFound += static_cast<size_t>(counters[c] - nLedgerCounters[c]);

        nLedgerCounters[c] = counters[c];
    }

    if (nFound == 0)
        return;

    // Several at once is a restore onto a board the same shape as the one it replaced, which the
    // length check above cannot see. Known state is already up to date, so this is the re-baseline.
    if (nFound > MAX_COMPLETIONS_PER_SAMPLE)
        return;

    Arm();
}

static bool IsReputationOnlyName(const char* pName)
{
    for (auto p : ReputationOnlyNames)
    {
        if (_stricmp(pName, p) == 0)
            return true;
    }
    return false;
}

// Every pattern here resolves exactly once against a retail Dunia.dll.
static void* Resolve(std::string_view bytes)
{
    auto pattern = dunia_pattern(bytes);
    return pattern.empty() ? nullptr : pattern.get_first(0);
}

class SaveOnMissionComplete
{
public:
    SaveOnMissionComplete()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            bConsoleAutosaves = JackalFixSettings.GetInt(PREF_CONSOLEAUTOSAVES) != 0;

            JackalFix::onIniFileChange() += []()
            {
                bConsoleAutosaves = JackalFixSettings.GetInt(PREF_CONSOLEAUTOSAVES) != 0;
            };

            //
            // ---- Arming ----
            //
            // PollLedger() covers every mission type. The rest either picks up the manager pointer
            // so the ledger can start, or is an older route kept as a second opinion.
            //

            // 0x10751200, the only assignment of selType; all six callers go through it. __thiscall
            // on the manager, new selType at [ESP+4] on entry, index at [ESP+8]. A mission ending is
            // {Story, Library, Buddy} -> None. FUN_10753060 (reset) and FUN_10752FA0 (per-faction
            // recount) also clear selType at load; the settle window in Arm() covers those.
            //
            // This is the ledger's bootstrap. The reset calls it before anything else in a session
            // can happen, so it is the earliest the manager pointer turns up.
            if (auto p = Resolve("83 EC 54 8B 44 24 58 53 8A 5C 24 60 56 8B F1 57 8D 4C 24 68"))
            {
                static auto SetCurrentMissionInfoHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto pMgr = static_cast<uintptr_t>(regs.ecx);
                    if (!pMgr)
                        return;

                    NoteMissionMgr(pMgr);

                    auto nOld = SelType(pMgr);
                    auto nNew = *(int32_t*)(static_cast<uintptr_t>(regs.esp) + 4);

                    if (IsRealMission(nOld) && nNew == SEL_NONE)
                        Arm();
                });
            }

            // 0x107532A0, CFCXMissionManager::MissionCompleted. __thiscall, MSVC8 std::string by
            // value at [esp+4], cleans 0x1C. Arms directly only before the manager pointer is known;
            // the name test cannot tell a first completion from a script re-announcing one.
            if (auto p = Resolve("83 EC 5C 53 55 56 57 33 ED 55 8D 44 24 24 8B F1 89 6C 24 18"))
            {
                static auto MissionCompletedHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto pMgr = static_cast<uintptr_t>(regs.ecx);
                    if (!pMgr)
                        return;

                    bool bKnewMgr = (pMissionMgr != 0);
                    NoteMissionMgr(pMgr);

                    char szName[128];
                    ReadDuniaString(static_cast<uintptr_t>(regs.esp) + 4, szName, sizeof(szName));

                    // These close out part of a mission and award reputation for it, recording
                    // nothing. Without the test, gating on the function alone produces spurious
                    // prompts.
                    if (IsReputationOnlyName(szName))
                        return;

                    // The ledger has this covered from here on.
                    if (bKnewMgr && bLedgerValid)
                        return;

                    Arm();
                });
            }

            // 0x10753E70, BypassMissionCompleted. Story and Library forward to MissionCompleted at
            // 0x10753F98. Buddy sets entry+0x40 itself at 0x10753EA9 / 0x10753EFC and never
            // delegates, which is why the hook below has to special-case it.
            if (auto p = Resolve("83 EC 08 53 55 56 8B F1 8B 86 74 01 00 00 83 F8 02 57"))
            {
                static auto BypassHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto pMgr = static_cast<uintptr_t>(regs.ecx);
                    if (!pMgr)
                        return;

                    NoteMissionMgr(pMgr);

                    if (SelType(pMgr) == SEL_BUDDY && !bLedgerValid)
                        Arm();
                });
            }

            // 0x101F1A30, CGameMission::OnStateChanged (vtable slot +0x2C). The runtime mission
            // system, separate from the board: CGameMission objects owned by CGameMissionMgr (global
            // 0x10FEFFF0, list at +0x1C, stride 0x28), state a bitmask at +0x08 set through
            // CBaseMission::SetState (vtable slot +0x24).
            //
            // Every transition from every source funnels through here, and only on a real change:
            // the MissionSetState script command, the Lua object API, native code and savegame
            // restore all land here, and rejected transitions never do. Nothing else needs hooking.
            // ECX = CGameMission*, [ESP+4] on entry = the new flag.
            if (auto p = Resolve("56 57 8B F9 8B 77 18 8B 47 1C 8D 04 80 8B CE 8D 14 81 3B F2 74 29"))
            {
                static auto StateChangedHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto nFlag = *(int32_t*)(static_cast<uintptr_t>(regs.esp) + 4);

                    // A live trace showed only 0, 1 and 2, but arming on 4 costs nothing.
                    if (nFlag == MISSIONSTATE_SUCCESS)
                    {
                        Arm();
                        return;
                    }

                    // Handler 0x10584DD0 only accepts flag 2 when bit 0 was set, so this mission was
                    // running and has ended. Too common to arm from alone: a save restore ends a
                    // pile in one tick. It only counts once the board reacts; see SelectMission.
                    if (nFlag == MISSIONSTATE_ENDED)
                        nMissionEndedAt = GetTickCount64();
                });
            }

            // 0x10755EE0, CFCXMissionManager::SelectMission, the board re-evaluating what a giver
            // can offer. Live trace over a story completion, one tick: two missions Ended,
            // SelectMission with selType back at 0, two more Enabled.
            //
            // Neither half is usable alone. Missions end constantly, and SelectMission also runs
            // when the player merely walks up to a giver. Combined, the route is specific but blind
            // to convoys and assassinations: those never touch the board, so it never refreshes.
            if (auto p = Resolve("83 EC 78 53 55 8B AC 24 84 00 00 00 56 57 33 FF 89 7D 00 8B F1"))
            {
                static auto SelectMissionHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    NoteMissionMgr(static_cast<uintptr_t>(regs.ecx));

                    if (!nMissionEndedAt || (GetTickCount64() - nMissionEndedAt) > MISSION_END_WINDOW_MS)
                        return;

                    nMissionEndedAt = 0;
                    Arm();
                });
            }

            // 0x107509F0, GiveMissionReward. Not a trigger, but __thiscall on the manager and runs
            // on paths the others do not.
            if (auto p = Resolve("83 EC 08 53 55 56 57 32 DB 68"))
            {
                static auto RewardHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    NoteMissionMgr(static_cast<uintptr_t>(regs.ecx));
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
            // Also the module's clock, and where the ledger is sampled.
            if (auto p = Resolve("80 BF FC 04 00 00 00 F3 0F 10 87 00 05 00 00 F3 0F 5C 44 24 60"))
            {
                static auto SavePointRequestHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto pPlayerFX = static_cast<uintptr_t>(regs.edi);
                    if (!pPlayerFX)
                        return;

                    auto nNow = GetTickCount64();
                    auto nGap = nLastTick ? (nNow - nLastTick) : 0;

                    // A new component is a new session. This, not the clock, invalidates the ledger.
                    if (pPlayerFX != pPlayerFXSeen)
                    {
                        pPlayerFXSeen = pPlayerFX;
                        nGameplaySince = nNow;

                        InvalidateLedger();

                        // Stops a prompt surfacing on the far side of a load.
                        nArmedAt = 0;
                    }
                    else if (nGap > GAMEPLAY_GAP_MS && nArmedAt)
                    {
                        // A hitch, not a load. The banner did not advance either, so push the delay
                        // out by the gap.
                        nArmedAt += nGap;
                    }
                    nLastTick = nNow;

                    // After nLastTick so Arm() sees a running tick rather than the gap just measured.
                    PollLedger();

                    if (!nArmedAt)
                        return;

                    // Checked here so switching the ini off mid-mission drops a pending request.
                    if (!bConsoleAutosaves)
                    {
                        nArmedAt = 0;
                        return;
                    }

                    if ((nNow - nArmedAt) < PROMPT_DELAY_MS)
                        return;

                    nArmedAt = 0;

                    *(float*)(pPlayerFX + PLAYERFX_SAVEPOINTCOOLDOWN) = 0.0f;
                    *(uint8_t*)(pPlayerFX + PLAYERFX_SAVEPOINTREQUEST) = 1;
                });
            }
        };
    }
} SaveOnMissionComplete;
