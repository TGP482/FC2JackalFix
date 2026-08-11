/*
  "Fixed a case where the player could no longer save their game after completing certain
  missions", from Scrubah's Patch. His version is a data edit to Domino/System/MissionCompleted.lua:

      if (self.Mission == "A1SM01") or (self.Mission == "A2SM08") then
          SetCinematicUIMode(1);
      end

  Both missions end on a scripted sequence that hides the gameplay HUD and does not always put it
  back. The pause menu Save Game entry and the quicksave binding both read that flag.

  SetCinematicUIMode is a Lua binding registered at 0x10725365. Its handler at 0x10724E20 forwards
  to 0x10724BF0, which plays the displayGameplayElements or hideGameplayElements transition and
  stores the argument as a byte at CFCXHudManager+0x158. The argument runs opposite to the
  binding's name: 1 shows the HUD and leaves cinematic mode.

  The trigger is CFCXMissionManager::MissionCompleted, 0x107532A0, which the box calls one line
  further down and which is handed the same "A1SM01" string the script tests. saveonmissioncomplete
  mid hooks the same entry, so this is a mid hook too and ReadDuniaString is duplicated here rather
  than shared.
*/

module;

#include <common.hxx>

export module cinematichud;

import common;
import dunia;

// 0x10724BF0. __cdecl, one stack argument, caller cleaned.
using SetCinematicUIMode_t = void(__cdecl*)(int32_t);

static SetCinematicUIMode_t SetCinematicUIMode = nullptr;

// Domino mission ids, as the script tests them rather than the engine names saveonmissioncomplete
// matches on. Kept to the two Scrubah names: firing on every completion would force the HUD up
// under a mission that ends inside a cinematic still meant to be running.
static const char* const StuckHudMissions[] =
{
    "A1SM01",   // Act 1, town escape
    "A2SM08",   // Act 2, warlord assassination
};

static bool IsStuckHudMission(const char* pszName)
{
    for (auto p : StuckHudMissions)
    {
        if (_stricmp(pszName, p) == 0)
            return true;
    }
    return false;
}

// MSVC8 std::string by value: size at +0x14, capacity at +0x18, buffer inline at +0x04 until it
// outgrows 16 bytes and +0x04 becomes a pointer to it.
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

class CinematicHud
{
public:
    CinematicHud()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // 0x10724BF0. Anchored on the prologue and the transition cache's refcount bump, with
            // both internal call displacements wildcarded.
            auto modePattern = dunia_pattern(
                "83 EC 24 53 56 57 E8 ? ? ? ? 83 40 08 01 89 44 24 10 8D 44 24 10 50 E8 ? ? ? ? "
                "83 C4 04 33 DB 84 C0");

            // 0x107532A0. __thiscall, MSVC8 std::string by value at [esp+4], callee cleans 0x1C.
            auto completedPattern = dunia_pattern("83 EC 5C 53 55 56 57 33 ED 55 8D 44 24 24 8B F1 89 6C 24 18");

            if (modePattern.empty() || completedPattern.empty())
                return;

            SetCinematicUIMode = reinterpret_cast<SetCinematicUIMode_t>(modePattern.get_first());

            static auto MissionCompletedHook = safetyhook::create_mid(completedPattern.get_first(), [](SafetyHookContext& regs)
            {
                char szName[64];
                ReadDuniaString(static_cast<uintptr_t>(regs.esp) + 4, szName, sizeof(szName));

                if (szName[0] == '\0' || !IsStuckHudMission(szName))
                    return;

                // Before the manager's own bookkeeping, matching the order in the script.
                SetCinematicUIMode(1);
            });
        };
    }
} CinematicHud;
