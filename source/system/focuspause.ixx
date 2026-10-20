module;

#include <common.hxx>
#include <atomic>

export module focuspause;

import common;
import dunia;
import settings;
import borderless;
import debug;

// Dunia mutes its sound and rumble on WM_ACTIVATE itself, but the game plays on. The engine thread
// waits in its own message pump, between frames, until the window is back in front. The engine's
// MaxDeltaTime clamp keeps the first frame back from jumping.
static std::atomic<bool> bPauseOnFocusLoss = true;

static constexpr DWORD nPausePollMs = 10;

// The mute only writes the master bus volume; the voices take it up on the next engine frames, so
// those run before the wait.
static constexpr ULONGLONG nMuteGraceMs = 100;

static ULONGLONG nUnfocusedSince = 0;

static SafetyHookInline PumpMessagesHook{};

static bool IsInBackground()
{
    if (!bPauseOnFocusLoss || JackalFixInMultiplayer())
        return false;

    auto hWnd = JackalFixGameWindow();
    if (hWnd == nullptr || !IsWindowVisible(hWnd))
        return false;

    return IsIconic(hWnd) || GetForegroundWindow() != hWnd;
}

// Engine thread only, so the timestamp needs no lock.
static bool UpdateShouldPause()
{
    if (!IsInBackground())
    {
        nUnfocusedSince = 0;
        return false;
    }

    const auto nNow = GetTickCount64();
    if (nUnfocusedSince == 0)
        nUnfocusedSince = nNow;

    return nNow - nUnfocusedSince >= nMuteGraceMs;
}

static bool QuitPending()
{
    MSG msg{};
    return PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE) != FALSE;
}

static bool __cdecl PumpMessages(bool bRepostQuit)
{
    auto bQuit = PumpMessagesHook.ccall<bool>(bRepostQuit);

    while (!bQuit && UpdateShouldPause() && !QuitPending())
    {
        Sleep(nPausePollMs);
        bQuit = PumpMessagesHook.ccall<bool>(bRepostQuit);
    }

    return bQuit;
}

class FocusPause
{
public:
    FocusPause()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            BindBool(bPauseOnFocusLoss, PREF_PAUSEONFOCUSLOSS);

            // The engine's PeekMessage loop; true on WM_QUIT.
            auto pPump = dunia_find("83 EC 38 53 56 8B 35 ? ? ? ? 57 6A 01 6A 00 6A 00 6A 00 8D 44 24 1C 50 FF D6 85 C0 74 39");
            if (pPump != nullptr)
                PumpMessagesHook = safetyhook::create_inline(pPump, PumpMessages);
        };
    }
} FocusPause;
