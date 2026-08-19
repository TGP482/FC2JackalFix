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
// before any window exists, so the rate comes from the primary display, which is the one the game
// lands on: Dunia creates its device on the primary adapter and borderless is positioned at 0,0.

static const char* const szRunGameExport = "?RunGame@@YA_NPAUHINSTANCE__@@PBD@Z";
static const char* const szMaxFpsSwitch = "-RenderProfile_MaxFps";

// gfx_MaxFps, registered by FUN_10402210 with a stock value of 200 written at 0x10403D7F. It takes
// a plain number and has no "off", so unlocked is a rate the engine cannot reach.
static constexpr int32_t nUnlockedFps = 9999;

// What EnumDisplaySettings answers for a display with no fixed rate. 0 and 1 both mean "the
// hardware default" rather than zero and one hertz.
static constexpr DWORD nUnknownRefreshRate = 1;
static constexpr int32_t nFallbackFps = 60;

enum MaxFrameRateSetting
{
    MAXFPS_UNLOCKED = 0,
    MAXFPS_DISPLAY = 1,
};

// Where the cap actually lives, so a change can take effect without a restart.
//
// MaxFps is a console variable of the RenderProfile group: a name, a type and a byte offset rather
// than storage. Its registration writes 0C8h into the offset field (Dunia+403D7F), and the group's
// integer setter is *(int*)(block + offset) = value (Dunia+781F0). The block is what the pointer at
// Dunia+1609560 refers to.
//
// Brightness, contrast and gamma are three floats at 12Ch, 130h and 134h of that same block, and
// CFCXOptionDisplayPage writes them straight in and then raises the render settings broadcast
// (Dunia+3F8AB0) with the block as its subject. That is the engine's own way of changing a render
// setting while it runs, and it is the one taken here.
static constexpr ptrdiff_t nProfileMaxFps = 0xC8;

// mov ecx,eax / call <set prompt enabled> / mov ecx,[<render profile>] / add esp,0Ch /
// jmp <render settings changed>. The tail of CFCXOptionDisplayPage's apply, which is the only
// place the block pointer and the broadcast appear together.
static const char* const szProfileTailPattern = "8B C8 E8 ? ? ? ? 8B 0D ? ? ? ? 83 C4 0C E9";
static constexpr ptrdiff_t nProfileTailPointer = 9;  // disp32 of mov ecx,[<render profile>]
static constexpr ptrdiff_t nProfileTailNotify = 16;  // the jmp itself

using NotifyRenderSettings_t = void(__fastcall*)(void* pProfile);

static void** ppRenderProfile = nullptr;
static NotifyRenderSettings_t NotifyRenderSettings = nullptr;

// The broadcast walks four listener lists and calls a virtual on every entry, so it is only raised
// from the thread the engine runs on. An ini edit made outside the game arrives on the file
// watcher's own thread; that case writes the field and leaves the announcement to whoever raises
// one next.
static DWORD nEngineThread = 0;

static SafetyHookInline RunGameHook{};

static bool ContainsSwitch(std::string_view cmdLine, std::string_view name)
{
    const auto it = std::search(cmdLine.begin(), cmdLine.end(), name.begin(), name.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });

    return it != cmdLine.end();
}

// ENUM_CURRENT_SETTINGS is the mode the display is running now, which is what a limiter has to
// match. Walking the adapter's supported modes instead gives the highest rate the display can do,
// and the two disagree on anything not set to its maximum.
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

// Does nothing before the render profile exists, which is every call made before the first frame.
// Start-up is the command line's job and this only covers what happens afterwards.
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
    // RunGame is the engine's own thread and never returns until the game does, so this is the
    // thread every frame is drawn on.
    nEngineThread = GetCurrentThreadId();

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

            // The command line is read once, so the switch above only settles the rate the game
            // starts at. Everything after that goes through the render profile directly.
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
