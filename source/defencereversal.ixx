/*
  "Fixed an issue where buddies wouldn't be considered missing if the player chose to help father
  Maliya at the church in town", from Scrubah's Patch. His version adds one line at the top of
  export:In() in Domino/User/a1sm03_defensereversal.churchassault.lua:

      GetBuddiesManager():SetDefenceRevesalBetrayedBuddies();

  A1SM03 branches: defend the church for Maliya, or take the other side. Only the second branch
  marks the buddies who sided against the player as missing.

  SetDefenceRevesalBetrayedBuddies is native at 0x10744A00, __fastcall on CBuddiesManager, no
  arguments. It walks the buddy array (base +0x44, count +0x48, stride 0xC0) and for every buddy
  that is live (+0x98 nonzero), belongs to the opposing faction (+0x1C) and is not already at
  status 2, writes status +0xA0 = 1 and clears +0xA4 and +0xA8..0xAB.

  The line after his enables the church attack mission, which is what this hooks instead:

    CGameMissionMgr::GetMission  0x101E44E0  latches the CGameMission* for the church path
    CBaseMission::SetState       0x10584D40  jump table over 0..8; arm 1 dispatches vtable +0x14,
                                             which the Lua Enable binding at 0x10585320 also
                                             calls, so flag 1 is Enable

  Neither alone is enough: GetMission would re-fire on any later query for that path and could
  re-mark a buddy who had since been rescued, and SetState cannot tell which mission it is because
  CGameMission+0x04 is an interned id rather than the path.

  The manager comes from a latch at 0x10722A96, just after the singleton resolve inside the
  GetBuddiesManager binding, which every buddy script goes through.
*/

module;

#include <common.hxx>

export module defencereversal;

import common;
import dunia;

// The mission the church branch enables, as the script spells it.
static constexpr const char* pszChurchAttackMission = "Missions/StoryMissions/A1SM03/A1SM03_ChurchAttack";

// CBaseMission::SetState arm 1, read off the jump table and confirmed against the Enable binding.
static constexpr int32_t MISSIONSTATE_STARTED = 1;

// 0x10744A00. __fastcall on CBuddiesManager, no stack arguments.
using SetDefenceRevesalBetrayedBuddies_t = void(__fastcall*)(void*, void*);

static SetDefenceRevesalBetrayedBuddies_t SetBetrayedBuddiesMissing = nullptr;

static void* pBuddiesMgr = nullptr;
static void* pChurchMission = nullptr;

// Paths reach GetMission with either separator and in any case, and the argument is a raw pointer
// with no length, so the compare folds both and is bounded.
static constexpr size_t nMaxPathLength = 260;

static bool PathIs(const char* pszPath, const char* pszExpected)
{
    if (pszPath == nullptr)
        return false;

    auto fold = [](uint8_t c) -> uint8_t
    {
        if (c == '\\') return static_cast<uint8_t>('/');
        return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + ('a' - 'A')) : c;
    };

    for (size_t i = 0; i < nMaxPathLength; ++i)
    {
        const auto a = static_cast<uint8_t>(pszPath[i]);
        const auto b = static_cast<uint8_t>(pszExpected[i]);

        if (a == 0 || b == 0)
            return a == b;

        if (fold(a) != fold(b))
            return false;
    }

    return false;
}

// One dword, handed straight to a string-to-buffer conversion, so it is either a char* or an MSVC8
// std::string*. Both are tried: a std::string under 16 characters keeps its text inline at +0x04,
// where a raw read would see pointer bytes instead.
static const char* PathFromArgument(const void* pArg)
{
    if (pArg == nullptr || IsBadReadPtr(pArg, 4))
        return nullptr;

    auto* pszDirect = static_cast<const char*>(pArg);
    if (!IsBadReadPtr(pszDirect, 1) && *pszDirect >= 0x20 && *pszDirect < 0x7F)
        return pszDirect;

    if (IsBadReadPtr(pArg, 0x1C))
        return nullptr;

    const auto nSize = *(const uint32_t*)((uintptr_t)pArg + 0x14);
    const auto nCapacity = *(const uint32_t*)((uintptr_t)pArg + 0x18);
    if (nSize == 0 || nSize > nMaxPathLength || nCapacity > 0x10000)
        return nullptr;

    auto* pszInline = (const char*)((uintptr_t)pArg + 4);
    return (nCapacity >= 16) ? *(const char* const*)pszInline : pszInline;
}

// __thiscall with one stack argument and a callee cleanup of four bytes.
static SafetyHookInline GetMissionHook{};

static void* __fastcall GetMission(void* pMgr, void* pEdx, const void* pPath)
{
    auto* pMission = GetMissionHook.fastcall<void*>(pMgr, pEdx, pPath);

    if (pMission != nullptr && PathIs(PathFromArgument(pPath), pszChurchAttackMission))
        pChurchMission = pMission;

    return pMission;
}

// Same shape. Ahead of the original, so the buddies move before the mission starts, which is the
// order the script has them in.
static SafetyHookInline SetStateHook{};

static void __fastcall SetState(void* pMission, void* pEdx, int32_t nFlag)
{
    if (pMission == pChurchMission && nFlag == MISSIONSTATE_STARTED
        && pBuddiesMgr != nullptr && SetBetrayedBuddiesMissing != nullptr)
    {
        SetBetrayedBuddiesMissing(pBuddiesMgr, nullptr);
    }

    SetStateHook.fastcall(pMission, pEdx, nFlag);
}

class DefenceReversal
{
public:
    DefenceReversal()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // 0x101E44E0. Anchored on the name intern and the array walk after it, with the intern
            // call displacement wildcarded.
            auto* pGetMission = dunia_find(
                "8B 44 24 04 56 8B F1 50 8D 4C 24 0C E8 ? ? ? ? 8B 4E 1C 85 C9 8B D0 5E 74 21");

            // 0x10584D40. The jump table's own address is absolute, so those four bytes are
            // wildcarded; the two arms after it are what make the pattern unique.
            auto* pSetState = dunia_find(
                "8B 44 24 04 83 F8 08 77 36 FF 24 85 ? ? ? ? 8B 01 8B 50 10 FF D2 C2 04 00 "
                "8B 01 8B 50 14 FF D2 C2 04 00");

            // 0x10744A00. Anchored on the lazy init guard of the faction manager it reads to decide
            // which side counts as betrayed.
            auto* pSetBuddies = dunia_find(
                "53 56 33 DB 39 1D ? ? ? ? 57 8B 3D ? ? ? ? 8B F1 75 07 33 C9 E8 ? ? ? ? "
                "6A 01 68 ? ? ? ? 8B CF E8 ? ? ? ? 80 78 40 01");

            // 0x10722A60, the GetBuddiesManager binding. Same singleton resolve shape as above.
            // +0x36 is the instruction after the resolve, where eax is the manager.
            auto* pGetBuddies = dunia_find(
                "56 8B 74 24 08 56 E8 ? ? ? ? 83 C4 04 85 C0 75 47 39 05 ? ? ? ? 57 8B 3D ? ? ? ? "
                "75 07 33 C9 E8 ? ? ? ? 6A 01 68 ? ? ? ? 8B CF E8 ? ? ? ?", 0x36);

            if (!pGetMission || !pSetState || !pSetBuddies || !pGetBuddies)
                return;

            SetBetrayedBuddiesMissing = reinterpret_cast<SetDefenceRevesalBetrayedBuddies_t>(pSetBuddies);

            static auto BuddiesMgrHook = safetyhook::create_mid(pGetBuddies, [](SafetyHookContext& regs)
            {
                pBuddiesMgr = reinterpret_cast<void*>(regs.eax);
            });

            GetMissionHook = safetyhook::create_inline(pGetMission, GetMission);
            SetStateHook = safetyhook::create_inline(pSetState, SetState);
        };
    }
} DefenceReversal;
