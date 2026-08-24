module;

#include <common.hxx>

export module cinematichud;

import common;
import dunia;

// SetCinematicUIMode. __cdecl, one stack argument, caller cleaned. The argument runs opposite to
// the name: 1 shows the HUD and leaves cinematic mode.
using SetCinematicUIMode_t = void(__cdecl*)(int32_t);

static SetCinematicUIMode_t SetCinematicUIMode = nullptr;

// Domino mission ids, as the script tests them. Both end on a scripted sequence that hides the HUD
// and does not always put it back, which also blocks saving; Scrubah's Patch fixes it in Lua.
// Only these two: every completion would force the HUD up under a cinematic still running.
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

class CinematicHud
{
public:
    CinematicHud()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Anchored on the prologue and the transition cache's refcount bump, call displacements wildcarded.
            auto* pMode = dunia_find(
                "83 EC 24 53 56 57 E8 ? ? ? ? 83 40 08 01 89 44 24 10 8D 44 24 10 50 E8 ? ? ? ? "
                "83 C4 04 33 DB 84 C0");

            // CFCXMissionManager::MissionCompleted. __thiscall, MSVC8 std::string by value at [esp+4].
            auto* pCompleted = dunia_find("83 EC 5C 53 55 56 57 33 ED 55 8D 44 24 24 8B F1 89 6C 24 18");

            if (!pMode || !pCompleted)
                return;

            SetCinematicUIMode = reinterpret_cast<SetCinematicUIMode_t>(pMode);

            static auto MissionCompletedHook = safetyhook::create_mid(pCompleted, [](SafetyHookContext& regs)
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
