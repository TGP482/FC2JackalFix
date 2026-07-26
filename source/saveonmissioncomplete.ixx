/*
  On the Xbox 360, finishing a mission popped a "Save Game" box offering "Save and continue" or
  "Continue without saving". The PC build never does it - but not because the feature was cut.

  The prompt is a game rules state, reached by a named event:

    GREventSavePointCheck -> CFCXGRStateSingleInGame::OnGREvent (0x107F8770), sets state+0x68
                          -> CFCXGRStateSavePointCheck::Enter   (0x107F5C70)
                          -> CSavePointCheckPage::OnEnter       (0x10850A90)

  CSavePointCheckPage::OnEnter builds exactly the 360's box - header MBOXHEADER_SAVEGAME, entries
  MBOXLISTSAVE_SAVEGAME and MBOXLISTSAVE_CONTINUENOSAVE out of the MessagesBoxListSavePointCheck
  table - and its selection callback (0x108509A0) either pushes CSavePointSaveGamePage or fires
  GREventNextState to drop straight back into gameplay. There is no "is the player at a save point"
  test anywhere in the page or in the state. All of the gating lives in whatever posts the event.

  The event name string (0x10E95B34) is materialised at seven sites, in five roles:

    CBedroll tick (safehouse bed)         0x106C184F
    CBedroll::OnMessage, "Save" branch    0x106C1DA3
    CPlayerSoundAndFXComponent::Update    0x106D3027, 0x106D3036
    SaveGame script command                0x1070E31C, 0x1070E32B
    OnGREvent consumer                     0x107F8958

  None of them is mission completion, so the 360's behaviour came from its mission scripts calling
  SaveGame() in the packed data. The 360 executable was opened alongside this one to settle it; the
  string is there at 0x820754DC but nothing in the disassembled portion of the image materialises it
  with a lis/addi pair, so its call sites cannot be enumerated the same way. Either way nothing is
  disabled on PC - there is simply no caller, and adding one is the whole feature.


  ---- What counts as a mission completing ----

  Everything the game records as a completed mission is written by CFCXMissionManager::MissionCompleted
  (0x107532A0), a script method. Reading it end to end gives the full taxonomy, in the order it tests:

    name == "SAVE_BUDDY" / "RESCUE_REUBEN"                       reputation +5,  records nothing
    name == "HOUSE_CLEANING1_BASE" / "HOUSE_CLEANING2_BASE"      reputation +10, records nothing
    name == "HOUSE_CLEANING1_SUBVERT" / "..2_SUBVERT"            reputation +20, records nothing
    name == "BUDDY_BETRAYAL" / "KILL_WARLORDS"                   reputation +20, records nothing
    name found in BSQMissions   (+0x130)                         entry+0x40 = 1
    name == "GRIN"                                               CompletedGrinMissions (+0x128)++
    name[2..3] == "CV"                                           CompletedConvoyMissions (+0x12A)++
    name[2..3] == "AS"                                           CompletedAssassinationMissions (+0x12C)++
    selType == Story   (2)                                       StoryMission   entry+0x40 = 1
    selType == Library (3)                                       LibraryMission entry+0x40 = 1
    selType == Buddy   (4)                                       BuddyMission   entry+0x40 = 1

  The first four groups return early: they are script calls that close out *part* of a mission and
  award reputation for it, and they are why gating on the function alone produces spurious prompts.

  The next four also return early - and that is the bug this module started out with. Buddy
  sidequests, the bonus (GRIN) missions, convoys and assassinations never reach the selType branches,
  so they never call SetCurrentMissionInfo(None) and the mission board never refreshes. Every arming
  route that keyed off the board therefore only ever saw story, library and buddy missions.

  So the primary route here does not watch functions at all. It watches the *record*: the three byte
  counters and the four arrays of completed flags listed above are, between them, everything the
  manager stores about missions the player has finished. Polling them once a frame and arming when
  the total goes up catches every mission type by construction, including anything reached by a path
  this comment has not anticipated - BypassMissionCompleted writing buddy flags directly, or
  SetLibraryMissionState, or a script route that does not exist in retail.

  The older function-shaped routes are kept alongside it. They latch into the same slot, so a
  completion seen twice still raises one prompt, and they are the fallback for the window at the very
  start of a session before the manager pointer is known.
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
// The layout below is read off the property registration in FUN_107567F0, which names every field,
// and off FUN_10753060, the reset, which walks each array with its stride in plain sight.
//

// "CurrentMissionInfo" is registered at +0x174 as an inline two-field struct: selType, an int32 enum
// at +0x174, and Index, a *byte* at +0x178. The enum's members are registered in order.
static constexpr uintptr_t MISSIONMGR_SELTYPE = 0x174;
static constexpr uintptr_t MISSIONMGR_INDEX = 0x178;
static constexpr uintptr_t MISSIONMGR_REPUTATION = 0x180;

static constexpr int32_t SEL_NONE = 0;
static constexpr int32_t SEL_LIBRARYOFFERED = 1;   // a list of offers, no single mission selected
static constexpr int32_t SEL_STORY = 2;
static constexpr int32_t SEL_LIBRARY = 3;
static constexpr int32_t SEL_BUDDY = 4;

// CFCXMission, the base every entry in every one of the four arrays derives from. Constructor at
// 0x1074EC40: vtable at +0, a byte at +4, an MSVC8 std::string at +0x08, a second at +0x24, and the
// completed flag at +0x40 zeroed last. The subclasses only differ below +0x40, which is why one
// pair of offsets reads all four arrays.
static constexpr uintptr_t MISSION_NAME = 0x08;
static constexpr uintptr_t MISSION_COMPLETED = 0x40;

struct MissionArrayDef
{
    uintptr_t nBase;    // offset of the pointer to the first entry
    uintptr_t nCount;   // offset of the int32 entry count
    size_t nStride;
    const char* pWhat;
};

// Bases, counts and strides all confirmed against the four walk loops in FUN_10753060.
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

// The three kinds MissionCompleted tallies instead of flagging, because they have no board entry to
// flag. Registered by name, so these are not guesses.
static constexpr MissionCounterDef MissionCounters[] =
{
    { 0x128, "bonus"         },   // CompletedGrinMissions
    { 0x12A, "convoy"        },   // CompletedConvoyMissions
    { 0x12C, "assassination" },   // CompletedAssassinationMissions
};
static constexpr size_t MISSION_COUNTERS = std::size(MissionCounters);

// An array that reports more than this has not been deserialised yet, or the pointer is stale.
// Retail's largest is well under a hundred.
static constexpr int32_t MAX_MISSION_ENTRIES = 512;

// The names MissionCompleted answers with reputation and nothing else. Complete as of 0x107532A0 -
// these are every early return above the first branch that writes something down. Only consulted by
// the fallback route, which runs before the manager pointer is known.
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

// CPlayerSoundAndFXComponent. +0x4FC is a pending "ask the game rules for a save point check"
// request and +0x500 is the cooldown that suppresses it; see the fire hook for the shape.
static constexpr uintptr_t PLAYERFX_SAVEPOINTREQUEST = 0x4FC;
static constexpr uintptr_t PLAYERFX_SAVEPOINTCOOLDOWN = 0x500;

// A gap longer than this in the player component's tick is worth noticing. It is NOT taken to mean
// a load: over ~25 minutes of play, 27 gaps were observed against only three distinct player
// components, so roughly eight in nine were ordinary frame hitches - streaming, an autosave, a disk
// stall. Treating those as a session change is how an armed prompt gets dropped two seconds after
// the mission that earned it. What a gap does mean is that the wall clock ran on while the game did
// not, so the prompt delay is pushed out to match.
static constexpr uint64_t GAMEPLAY_GAP_MS = 1000;

// How long gameplay has to have been running before a recorded completion is believed. The manager
// is reset and deserialised at the start of a session, and both look like a mission ending.
static constexpr uint64_t GAMEPLAY_SETTLE_MS = 3000;

// Missions conclude one at a time. A sample where several appear at once is the board being
// restored, not a burst of gameplay - so it re-baselines instead of prompting. Two rather than one
// because a script closing out a mission and its follow-up in the same frame is at least plausible,
// and a missed prompt is worse than an early one.
static constexpr size_t MAX_COMPLETIONS_PER_SAMPLE = 2;

// A mission concluding shows up as a CGameMission going Started -> Ended and the mission board
// re-evaluating its offers immediately afterwards. This is how long the two are allowed to be apart.
static constexpr uint64_t MISSION_END_WINDOW_MS = 2000;

// How long to sit on an armed completion before asking for the prompt. The box would otherwise land
// on the same frame the mission ends, on top of the MISSION COMPLETE banner and the objective
// update that follows it. Two seconds lets those read before the game stops for input. This is a
// floor, not a schedule - the engine's busy gate can still hold the prompt back further.
static constexpr uint64_t PROMPT_DELAY_MS = 2000;

// Zero means nothing pending. Doubles as the timestamp the delay above is measured from.
static uint64_t nArmedAt = 0;
static uint64_t nMissionEndedAt = 0;

static uint64_t nLastTick = 0;
static uint64_t nGameplaySince = 0;

// The CPlayerSoundAndFXComponent the tick last ran on. It is rebuilt with the player, so a change
// here is a session change and a gap in the clock is not.
static uintptr_t pPlayerFXSeen = 0;

// CBaseMission::SetState (0x10584D40) is a jump table over 0..8 with the odd values falling through
// to a no-op, which is the giveaway that the state is a *bitmask*, not an ordinal. Values 1 and 2
// are confirmed by name - they are the Lua Enable and Disable methods (thunks 0x10585320 and
// 0x10585340). 4 and 8 are the outcome pair and their names are not in the DLL; the enum members
// live in the packed data. 4 before 8 in both the jump table and the vtable makes success/failure
// the obvious reading, and a live trace over a completed mission agreed.
static constexpr int32_t MISSIONSTATE_NONE = 0;
static constexpr int32_t MISSIONSTATE_STARTED = 1;   // confirmed: Lua Enable
static constexpr int32_t MISSIONSTATE_ENDED = 2;     // confirmed: Lua Disable
static constexpr int32_t MISSIONSTATE_SUCCESS = 4;   // inferred
static constexpr int32_t MISSIONSTATE_FAILED = 8;    // inferred

// The three kinds that name a single mission. LibraryOffered (1) is a *list* of offers and None (0)
// is nothing selected, so neither is a mission that can conclude.
static bool IsRealMission(int32_t nSelType)
{
    return nSelType == SEL_STORY || nSelType == SEL_LIBRARY || nSelType == SEL_BUDDY;
}

// MSVC8 std::string: proxy at +0, 16 byte SSO buffer at +4 which becomes a pointer once the
// capacity at +0x18 reaches 16, length at +0x14. Belt and braces on the reads - this runs on
// arguments whose type is inferred from disassembly, and a read must not be what crashes.
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

// Every arming route funnels through here. One slot, so several routes firing for the same
// completion still raise one prompt - and the engine puts a five second cooldown on the request slot
// after it posts, which covers the rest.
static void Arm()
{
    auto nNow = GetTickCount64();

    // Called from the tick itself, where nLastTick was set a moment ago, and from hooks that can run
    // while the game is stalled. The second case is the one this rejects: a request made mid-stall
    // has no idea whether the session it belongs to still exists.
    if (nLastTick == 0 || (nNow - nLastTick) >= GAMEPLAY_GAP_MS)
        return;

    // The manager is reset and deserialised at the start of a session, and both look like missions
    // ending. Hitches no longer restart this clock, so it only ever covers a genuine session start.
    if ((nNow - nGameplaySince) < GAMEPLAY_SETTLE_MS)
        return;

    // First arm wins. A second route firing during the delay would otherwise keep pushing the
    // prompt back, and the delay is meant to be measured from the completion.
    if (nArmedAt)
        return;

    nArmedAt = nNow;
}

//
// ---- Route 1: the ledger ----
//
// The mission board's own record of what the player has finished, sampled once a frame. Nothing here
// knows or cares which function did the writing, which is the entire point: convoys, assassinations,
// buddy sidequests and the bonus missions all return out of MissionCompleted long before the board
// state that the other routes watch is touched, and they still land here.
//

static uintptr_t pMissionMgr = 0;

static bool bLedgerValid = false;
static uint8_t nLedgerCounters[MISSION_COUNTERS] = {};
static std::vector<uint8_t> LedgerFlags[MISSION_ARRAYS];

// Called whenever the manager is seen as a `this`. Cheap enough to do unconditionally, and the first
// one to arrive is what lets the ledger start sampling.
static void NoteMissionMgr(uintptr_t p)
{
    if (!p || p == pMissionMgr)
        return;

    // A different manager means a different session; nothing sampled off the old one is comparable.
    pMissionMgr = p;
    bLedgerValid = false;
}

static void InvalidateLedger()
{
    bLedgerValid = false;
}

// Scratch for the sample being taken. Function-static rather than local: this runs once per frame,
// and four fresh vectors a frame is four allocations a frame for the life of the process. They only
// ever grow to the size of the board, which is under a hundred entries in retail.
static std::vector<uint8_t> SampleFlags[MISSION_ARRAYS];

// One array's completed flags, or false if the array is not in a state worth reading. Ranges are
// probed rather than trusted: this runs every frame, including through level changes, and the base
// pointers are null for part of that.
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

// Runs from the per-frame hook. Arms on any newly recorded completion; silently re-baselines on
// anything that looks like the record being rebuilt rather than added to.
static void PollLedger()
{
    // Everything read below lives inside the manager's first 0x184 bytes, so one probe covers the lot.
    if (!pMissionMgr || IsBadReadPtr((void*)pMissionMgr, MISSIONMGR_REPUTATION + sizeof(int32_t)))
        return;

    for (size_t a = 0; a < MISSION_ARRAYS; a++)
    {
        // Half a ledger is worse than none: it would arm on the arrays that did read the next time
        // the rest came back. Wait for a frame where all four are readable.
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

    // Collected first and acted on second. A restore that lands on a same-length board would
    // otherwise have raised a prompt on the first newly-set flag before the count that identifies it
    // as a restore had finished adding up. The known state is brought up to date either way - the
    // only question is whether any of it is worth prompting for.
    size_t nFound = 0;

    // The arrays. A length change means the board was rebuilt underneath us - a new act, a different
    // save - so the flags either side of it are not the same missions and cannot be compared.
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

            // Cleared rather than set is a reload, or a script undoing itself; not a completion.
            if (sample[i])
                nFound++;
        }
    }

    // The counters. These are the three kinds with no board entry to flag, so a tally is all there is.
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

    // Several at once is a savegame being restored onto a board that happens to be the same shape as
    // the one it replaced - the case the length check above cannot see. Everything is already up to
    // date, so saying nothing here is exactly a re-baseline.
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

// Every pattern here was checked against a retail Dunia.dll and resolves exactly once.
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
            // The ledger poll in PollLedger() is the route that covers every mission type. Everything
            // below is either a way of getting hold of the manager pointer so the ledger can start,
            // or one of the older function-shaped routes kept as a second opinion. They all latch
            // into the same slot.
            //

            // 0x10751200. The only thing in the binary that assigns selType, and all six callers go
            // through it:
            //
            //   10751200  SUB ESP,0x54
            //   10751203  MOV EAX, [ESP+0x58]              ; new selType
            //   10751208  MOV BL,  [ESP+0x60]              ; new index
            //   1075121F  MOV dword ptr [ESI+0x174], EAX
            //   10751225  MOV byte  ptr [ESI+0x178], BL
            //
            // __thiscall, so at entry ECX is the manager, [ESP] the return address, [ESP+4] the new
            // selType and [ESP+8] the index. A mission ending is {Story, Library, Buddy} -> None.
            //
            // Two callers clear selType for reasons that are not a completion - FUN_10753060, the
            // full reset, and FUN_10752FA0, the per-faction recount. Both run at load, which is what
            // the settle window in Arm() is for.
            //
            // This is also the earliest the manager pointer turns up in a session: the reset calls it
            // before anything else can happen.
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

            // 0x107532A0, CFCXMissionManager::MissionCompleted. __thiscall, one MSVC8 std::string by
            // value at [esp+4], cleans 0x1C.
            //
            // This is where the whole taxonomy at the top of the file lives. It arms directly, but
            // only in the window before the manager pointer is known - which is the first frames of
            // a session, when Arm() would refuse anyway. Once the ledger is running this is left
            // alone, because the name test cannot tell a first completion from a script
            // re-announcing one, and the ledger can.
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

                    // Closes out part of a mission and awards reputation for it; records nothing.
                    if (IsReputationOnlyName(szName))
                        return;

                    // The ledger has this covered from here on, and covers it more precisely.
                    if (bKnewMgr && bLedgerValid)
                        return;

                    Arm();
                });
            }

            // 0x10753E70, BypassMissionCompleted. For Story and Library it forwards to
            // MissionCompleted at 0x10753F98. For Buddy it sets entry+0x40 itself at 0x10753EA9 /
            // 0x10753EFC and never delegates - the ledger sees that write like any other, which is
            // exactly why the ledger is the primary route.
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

            // 0x101F1A30. The runtime mission system, which is a different thing entirely from the
            // mission board. CFCXMissionManager is what a giver can offer and what the player has
            // ticked off. The missions that actually run are CGameMission objects owned by
            // CGameMissionMgr (global 0x10FEFFF0, list at +0x1C, stride 0x28), and their state is a
            // bitmask at +0x08 driven through CBaseMission::SetState (0x10584D40, vtable slot +0x24).
            //
            // Every transition from every source - the MissionSetState script command, the Lua object
            // API, native code, and savegame restore - funnels into CGameMission::OnStateChanged
            // (vtable slot +0x2C), and only on a real change; rejected transitions never reach it.
            //
            //   101F1A30  PUSH ESI / PUSH EDI / MOV EDI,ECX      ; ECX = CGameMission*
            //             ...walk the listener vector at +0x18...
            //             [ESP+4] on entry = the new flag
            if (auto p = Resolve("56 57 8B F9 8B 77 18 8B 47 1C 8D 04 80 8B CE 8D 14 81 3B F2 74 29"))
            {
                static auto StateChangedHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto nFlag = *(int32_t*)(static_cast<uintptr_t>(regs.esp) + 4);

                    // Retail never uses the outcome bits - a live trace over a completed mission
                    // showed only 0, 1 and 2 - but arming on 4 costs nothing if some mission does.
                    if (nFlag == MISSIONSTATE_SUCCESS)
                    {
                        Arm();
                        return;
                    }

                    // Reaching here with flag 2 means the transition was accepted, and handler
                    // 0x10584DD0 only accepts it when bit 0 was set - so this mission really was
                    // running and has now ended. Far too common on its own to arm from: the world
                    // enables and disables dozens of these, and a save restore ends a pile of them
                    // in one tick. It only counts once the board reacts. See the SelectMission hook.
                    if (nFlag == MISSIONSTATE_ENDED)
                        nMissionEndedAt = GetTickCount64();
                });
            }

            // 0x10755EE0, CFCXMissionManager::SelectMission - the mission board re-evaluating what a
            // giver can offer. A live trace over a real story completion caught the whole signature
            // in one tick: two missions Ended, then SelectMission with selType back at 0, then two
            // more missions Enabled.
            //
            // Neither half is usable alone. Missions end constantly - a save restore ends a dozen in
            // one tick and the world enables and disables safehouse AI missions as the player moves -
            // and SelectMission also runs when the player merely walks up to a giver. Together they
            // are specific.
            //
            // This is the route that made story, library and buddy missions work before the ledger
            // existed, and it is also the route that could never see a convoy or an assassination:
            // those never touch the board, so it never refreshes.
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

            // 0x107509F0, GiveMissionReward - the diamonds. Not a trigger on its own account, but it
            // is __thiscall on the manager and it runs on plenty of paths the others do not, so it is
            // a useful place to pick the pointer up.
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
            // Posting GREventSavePointCheck directly would work - the dispatcher at 0x104DBA30 is
            // reachable and the event is a plain std::string plus an entity id - but a mission
            // usually concludes underneath a cutscene, a dialogue or a fade, and a raw post would
            // land in the middle of it. CPlayerSoundAndFXComponent::Update (0x106D2C20) already
            // carries a request slot that solves this, and the safehouse path uses it:
            //
            //   106D2F42  CMP    byte ptr [EDI+0x4FC], 0     ; request pending?
            //   106D2F49  MOVSS  XMM0, [EDI+0x500]           ; cooldown
            //   106D2F51  SUBSS  XMM0, [ESP+0x60]            ; -= dt
            //   106D2F57  MOVSS  [EDI+0x500], XMM0
            //   106D2F5F  JZ     0x106D3135                  ; no request, done
            //   106D2F65  COMISS XMM0, 0.0
            //   106D2F6C  JBE    0x106D2F75                  ; cooldown expired, let it through
            //   106D2F6E  MOV    byte ptr [EDI+0x4FC], 0     ; still cooling down, drop it
            //             ...three gates...
            //             post GREventSavePointCheck; [EDI+0x500] = 5.0f; [EDI+0x4FC] = 0
            //
            // The third gate, DAT_11649EF4's vtable +0x14 at 0x106D2FCE, is a busy test, and failing
            // it jumps to the tail *without* clearing the flag - so the component quietly retries on
            // the next tick until the game is ready. Setting the flag here buys all of that free.
            //
            // The hook sits on the CMP itself. Writing the flag before the relocated instruction
            // re-executes means this tick already sees it, and zeroing the cooldown first stops the
            // 0x106D2F6E branch from throwing the request away.
            //
            // This is also the module's clock. It runs once per frame for as long as the player
            // exists, so the gap between two calls is what tells running gameplay from a session that
            // has just come back from a load - and it is where the ledger is sampled.
            if (auto p = Resolve("80 BF FC 04 00 00 00 F3 0F 10 87 00 05 00 00 F3 0F 5C 44 24 60"))
            {
                static auto SavePointRequestHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
                {
                    auto pPlayerFX = static_cast<uintptr_t>(regs.edi);
                    if (!pPlayerFX)
                        return;

                    auto nNow = GetTickCount64();
                    auto nGap = nLastTick ? (nNow - nLastTick) : 0;

                    // The component is destroyed and rebuilt with the player, so a new one is a new
                    // session - a level change, or a savegame loaded over the top of this one. That,
                    // and not the clock, is what says nothing sampled earlier is still comparable.
                    if (pPlayerFX != pPlayerFXSeen)
                    {
                        pPlayerFXSeen = pPlayerFX;
                        nGameplaySince = nNow;

                        InvalidateLedger();

                        // Dropping the arm here stops a prompt from surfacing on the far side of a
                        // load, detached from the mission it belonged to.
                        nArmedAt = 0;
                    }
                    else if (nGap > GAMEPLAY_GAP_MS && nArmedAt)
                    {
                        // Same player, so the same session: a hitch, not a load. Nothing is dropped
                        // and nothing is re-baselined. The delay is meant to give the MISSION
                        // COMPLETE banner time to read, and the banner did not advance either, so it
                        // is pushed out by however long the game was gone.
                        nArmedAt += nGap;
                    }
                    nLastTick = nNow;

                    // Route 1, and after nLastTick so that Arm() sees a tick that is running rather
                    // than the gap it just measured.
                    PollLedger();

                    if (!nArmedAt)
                        return;

                    // Checked here rather than at arming time, so switching the ini off mid-mission
                    // does not leave a request waiting to go off later.
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
