/* Based on Scrubah's Patch */

module;

#include <common.hxx>

export module defencereversal;

import common;
import dunia;

// The mission the church branch enables, as the script spells it.
static constexpr const char* pszChurchAttackMission = "Missions/StoryMissions/A1SM03/A1SM03_ChurchAttack";

// CBaseMission::SetState arm 1.
static constexpr int32_t MISSIONSTATE_STARTED = 1;

// Five bindings share this function, but their globals are relocated and cannot be in the pattern.
static constexpr ptrdiff_t nResolveGuardOperand = 0x14;
static constexpr ptrdiff_t nManagerInEax = 0x36;
static constexpr uintptr_t nBuddiesGuardRva = 0x164A474;

// __fastcall on CBuddiesManager, no stack arguments.
using SetDefenceRevesalBetrayedBuddies_t = void(__fastcall*)(void*, void*);

static SetDefenceRevesalBetrayedBuddies_t SetBetrayedBuddiesMissing = nullptr;

static void* pBuddiesMgr = nullptr;
static void* pChurchMission = nullptr;

// Paths reach GetMission with either separator and in any case, so the compare folds both; the
// argument has no length, hence bounded.
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

// One dword, either a char* or an MSVC8 std::string*; both are tried, a std::string under 16
// characters keeping its text inline at +0x04.
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

// Same shape. Ahead of the original, so the buddies move before the mission starts, as the script has it.
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
            // GetMission, anchored on the name intern and the array walk after it.
            auto* pGetMission = dunia_find(
                "8B 44 24 04 56 8B F1 50 8D 4C 24 0C E8 ? ? ? ? 8B 4E 1C 85 C9 8B D0 5E 74 21");

            // SetState; the jump table address is wildcarded, the two arms after it make it unique.
            auto* pSetState = dunia_find(
                "8B 44 24 04 83 F8 08 77 36 FF 24 85 ? ? ? ? 8B 01 8B 50 10 FF D2 C2 04 00 "
                "8B 01 8B 50 14 FF D2 C2 04 00");

            // SetDefenceRevesalBetrayedBuddies, anchored on the faction manager's lazy init guard.
            auto* pSetBuddies = dunia_find(
                "53 56 33 DB 39 1D ? ? ? ? 57 8B 3D ? ? ? ? 8B F1 75 07 33 C9 E8 ? ? ? ? "
                "6A 01 68 ? ? ? ? 8B CF E8 ? ? ? ? 80 78 40 01");

            // The GetBuddiesManager binding; +0x36 is after the resolve, where eax is the manager.
            auto buddiesPattern = dunia_pattern(
                "56 8B 74 24 08 56 E8 ? ? ? ? 83 C4 04 85 C0 75 47 39 05 ? ? ? ? 57 8B 3D ? ? ? ? "
                "75 07 33 C9 E8 ? ? ? ? 6A 01 68 ? ? ? ? 8B CF E8 ? ? ? ?");

            const auto nBuddiesGuard = reinterpret_cast<uintptr_t>(hDunia) + nBuddiesGuardRva;

            void* pGetBuddies = nullptr;
            for (size_t i = 0; i < buddiesPattern.size(); ++i)
            {
                auto* pBinding = buddiesPattern.get(i).get<uint8_t>(0);
                if (*reinterpret_cast<uintptr_t*>(pBinding + nResolveGuardOperand) != nBuddiesGuard)
                    continue;

                pGetBuddies = pBinding + nManagerInEax;
                break;
            }

            if (!pGetMission || !pSetState || !pSetBuddies || !pGetBuddies)
                return;

            SetBetrayedBuddiesMissing = reinterpret_cast<SetDefenceRevesalBetrayedBuddies_t>(pSetBuddies);

            static auto BuddiesMgrHook = safetyhook::create_mid(pGetBuddies, [](SafetyHookContext& regs)
            {
                pBuddiesMgr = reinterpret_cast<void*>(regs.eax);
            });

            // Both are needed: GetMission alone re-fires on any later query for the path, SetState
            // alone cannot tell which mission it is.
            GetMissionHook = safetyhook::create_inline(pGetMission, GetMission);
            SetStateHook = safetyhook::create_inline(pSetState, SetState);
        };
    }
} DefenceReversal;
