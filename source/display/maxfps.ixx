module;

#include <common.hxx>

export module maxfps;

import common;
import dunia;
import settings;

static const char* const szRunGameExport = "?RunGame@@YA_NPAUHINSTANCE__@@PBD@Z";
static const char* const szMaxFpsSwitch = "-RenderProfile_MaxFps";

// gfx_MaxFps takes a plain number and has no "off", so unlocked is a rate the engine cannot reach.
static constexpr int32_t nUnlockedFps = 9999;

// What EnumDisplaySettings answers for a display with no fixed rate; 0 and 1 both mean "default".
static constexpr DWORD nUnknownRefreshRate = 1;
static constexpr int32_t nFallbackFps = 60;

enum MaxFrameRateSetting
{
    MAXFPS_UNLOCKED = 0,
    MAXFPS_DISPLAY = 1,
};

// Where the cap actually lives, so a change can take effect without a restart. MaxFps is a
// RenderProfile console variable at offset 0C8h into the block held by the render profile global.
// Writing the field then raising the render settings broadcast is CFCXOptionDisplayPage's own route.
static constexpr ptrdiff_t nProfileMaxFps = 0xC8;

// Tail of CFCXOptionDisplayPage's apply, the only place the block pointer and the broadcast meet.
static const char* const szProfileTailPattern = "8B C8 E8 ? ? ? ? 8B 0D ? ? ? ? 83 C4 0C E9";
static constexpr ptrdiff_t nProfileTailPointer = 9;  // disp32 of mov ecx,[<render profile>]
static constexpr ptrdiff_t nProfileTailNotify = 16;  // the jmp itself

using NotifyRenderSettings_t = void(__fastcall*)(void* pProfile);

static void** ppRenderProfile = nullptr;
static NotifyRenderSettings_t NotifyRenderSettings = nullptr;

// The broadcast walks four listener lists calling a virtual on every entry, so raise it only from the
// engine thread. An ini edit arrives on the watcher thread and just writes the field.
static DWORD nEngineThread = 0;

static SafetyHookInline RunGameHook{};

static bool ContainsSwitch(std::string_view cmdLine, std::string_view name)
{
    const auto it = std::search(cmdLine.begin(), cmdLine.end(), name.begin(), name.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });

    return it != cmdLine.end();
}

// ENUM_CURRENT_SETTINGS is the mode the display is running now, which is what a limiter has to match;
// the adapter's supported-mode list would give its highest rate instead. Primary display: RunGame
// runs before any window exists, and Dunia creates its device there anyway.
static int32_t GetDisplayRefreshRate()
{
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);

    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        return nFallbackFps;

    if ((dm.dmFields & DM_DISPLAYFREQUENCY) == 0 || dm.dmDisplayFrequency <= nUnknownRefreshRate)
        return nFallbackFps;

    // Whole hertz, so a 59.94Hz mode arrives as 59 and the cap lands just under the refresh.
    return static_cast<int32_t>(dm.dmDisplayFrequency);
}

// MaxFrameRate 1 resolves here, not in the ini reader, so the stored setting stays the number the
// player wrote.
static int32_t ResolveMaxFps()
{
    const auto nSetting = JackalFixSettings.GetInt(PREF_MAXFRAMERATE);

    if (nSetting == MAXFPS_UNLOCKED)
        return nUnlockedFps;

    if (nSetting == MAXFPS_DISPLAY)
        return GetDisplayRefreshRate();

    return nSetting;
}

// No-op before the render profile exists; start-up is the command line's job.
static void ApplyMaxFps()
{
    if (ppRenderProfile == nullptr)
        return;

    auto pProfile = *ppRenderProfile;
    if (pProfile == nullptr)
        return;

    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pProfile) + nProfileMaxFps) = ResolveMaxFps();

    if (NotifyRenderSettings != nullptr && GetCurrentThreadId() == nEngineThread)
        NotifyRenderSettings(pProfile);
}

static bool __cdecl RunGame(HINSTANCE hInstance, const char* pCmdLine)
{
    // RunGame is the engine thread and does not return until the game does.
    nEngineThread = GetCurrentThreadId();

    // Keep alive for the duration of RunGame.
    static std::string cmdLine;

    cmdLine = pCmdLine ? pCmdLine : "";

    // The engine already handles the switch, so append it rather than patch; a user argument wins.
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

            // The command line is read once, so it only sets the starting rate; later changes go through here.
            auto tail = dunia_pattern(szProfileTailPattern);
            if (!tail.empty())
            {
                ppRenderProfile = *tail.get_first<void**>(nProfileTailPointer);

                auto pJump = tail.get_first<uint8_t>(nProfileTailNotify);
                NotifyRenderSettings = reinterpret_cast<NotifyRenderSettings_t>(
                    pJump + 5 + *reinterpret_cast<int32_t*>(pJump + 1));

                JackalFix::onIniFileChange() += []() { ApplyMaxFps(); };
            }
        };
    }
} MaxFrameRate;
