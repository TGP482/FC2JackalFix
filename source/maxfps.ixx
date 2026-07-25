module;

#include <common.hxx>

export module maxfps;

import common;
import dunia;
import settings;

// MaxFps is already supported through -RenderProfile_MaxFps. Append the switch to the existing
// command line instead of patching the engine. The value is not persisted and an explicit user
// argument takes priority.

static const char* const szRunGameExport = "?RunGame@@YA_NPAUHINSTANCE__@@PBD@Z";
static const char* const szMaxFpsSwitch = "-RenderProfile_MaxFps";

static SafetyHookInline RunGameHook{};

static bool ContainsSwitch(std::string_view cmdLine, std::string_view name)
{
    const auto it = std::search(cmdLine.begin(), cmdLine.end(), name.begin(), name.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });

    return it != cmdLine.end();
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
        cmdLine += std::to_string(JackalFixSettings.GetInt(PREF_MAXFRAMERATE));
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
        };
    }
} MaxFrameRate;
