module;

#include <common.hxx>

export module maxfps;

import common;
import dunia;
import settings;

// The engine already handles -RenderProfile_MaxFps, so append the switch to the command line
// instead of patching. An explicit user argument takes priority.
//
// MaxFrameRate 1 asks for whatever the display is running at, resolved here rather than in the ini
// reader so the stored setting stays the number the player wrote. RunGame consumes the command line
// before any window exists, so the rate comes from the primary display. Dunia creates its device on
// the primary adapter and borderless is positioned at 0,0, so that is the display the game lands on
// unless the player moves the window off it.

static const char* const szRunGameExport = "?RunGame@@YA_NPAUHINSTANCE__@@PBD@Z";
static const char* const szMaxFpsSwitch = "-RenderProfile_MaxFps";

// gfx_MaxFps, registered by FUN_10402210 with a stock value of 200 written at 0x10403D7F. It takes
// a plain number and has no "off", so unlocked is a rate the engine cannot reach.
static constexpr int32_t nUnlockedFps = 9999;

// What EnumDisplaySettings answers for a display with no fixed rate. 0 and 1 both mean "the
// hardware default", not zero and one hertz.
static constexpr DWORD nUnknownRefreshRate = 1;
static constexpr int32_t nFallbackFps = 60;

enum MaxFrameRateSetting
{
    MAXFPS_UNLOCKED = 0,
    MAXFPS_DISPLAY = 1,
};

static SafetyHookInline RunGameHook{};

static bool ContainsSwitch(std::string_view cmdLine, std::string_view name)
{
    const auto it = std::search(cmdLine.begin(), cmdLine.end(), name.begin(), name.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });

    return it != cmdLine.end();
}

// ENUM_CURRENT_SETTINGS is the mode the display is running now, which is what a limiter has to
// match. Walking the adapter's supported modes instead would give the highest rate the display can
// do, and the two disagree on anything the player has not set to its maximum.
static int32_t GetDisplayRefreshRate()
{
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);

    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        return nFallbackFps;

    if ((dm.dmFields & DM_DISPLAYFREQUENCY) == 0 || dm.dmDisplayFrequency <= nUnknownRefreshRate)
        return nFallbackFps;

    // Whole hertz, so a 59.94Hz mode arrives as 59 and the cap lands just under the refresh rather
    // than just over it.
    return static_cast<int32_t>(dm.dmDisplayFrequency);
}

static int32_t ResolveMaxFps()
{
    const auto nSetting = JackalFixSettings.GetInt(PREF_MAXFRAMERATE);

    if (nSetting == MAXFPS_UNLOCKED)
        return nUnlockedFps;

    if (nSetting == MAXFPS_DISPLAY)
        return GetDisplayRefreshRate();

    return nSetting;
}

static bool __cdecl RunGame(HINSTANCE hInstance, const char* pCmdLine)
{
    // Keep alive for the duration of RunGame.
    static std::string cmdLine;

    cmdLine = pCmdLine ? pCmdLine : "";

    if (!ContainsSwitch(cmdLine, szMaxFpsSwitch))
    {
        cmdLine += ' ';
        cmdLine += szMaxFpsSwitch;
        cmdLine += ' ';
        cmdLine += std::to_string(ResolveMaxFps());
    }

    return RunGameHook.ccall<bool>(hInstance, cmdLine.c_str());
}

class MaxFrameRate
{
public:
    MaxFrameRate()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            auto pRunGame = GetProcAddress(hDunia, szRunGameExport);
            if (!pRunGame)
                return;

            RunGameHook = safetyhook::create_inline(pRunGame, RunGame);

            // The command line is read once, so an ini change lands on the next launch. Nothing is
            // registered on the file watch for that reason.
        };
    }
} MaxFrameRate;
