module;

#include <common.hxx>

export module borderless;

import common;
import dunia;
import settings;

// Dunia already ships a borderless mode. InitDuniaEngine tests its command line for "-borderless"
// and hands the result to the window creator, which picks the window style from it:
//
//   set     0x80000000   WS_POPUP
//   clear   0x00CA0000   WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
//
// The switch is undocumented and has no entry in the video options. It is also only half of what
// borderless usually means: a window style is irrelevant while the device is fullscreen exclusive,
// and the window is sized to the chosen resolution and centred rather than covering the display. So
// three things move together here, each of them through the game's own code rather than around it.
//
//   1. The "-borderless" test result, forced true, taking the shipped WS_POPUP path. The style then
//      sticks for good: the resize helper reads the current style back with GetWindowLongA before
//      adjusting, and Dunia never imports SetWindowLongA at all, so nothing can restore the border.
//   2. The fullscreen byte in the renderer's per-run device config, forced clear. The game derives
//      D3DPRESENT_PARAMETERS.Windowed, FullScreen_RefreshRateInHz and PresentationInterval from that
//      one byte, so clearing it produces exactly the coherent windowed state a windowed profile
//      would have produced - rather than forcing Windowed on its own and leaving a non-zero refresh
//      rate behind, which D3D9 rejects.
//   3. The window rect, overridden to cover the primary display from 0,0.
//
// None of this writes the user's saved video options. The fullscreen byte lives in the renderer's
// own config struct, not in the serialised profile, so turning the option off restores stock
// behavior with nothing left behind.
//
// Two consequences worth knowing. The backbuffer still follows the in-game Resolution and D3D9
// stretches it to the client area while windowed, so a resolution below the desktop is scaled up to
// fill the screen rather than letterboxed - set them equal for a 1:1 image. And the Brightness,
// Contrast and Gamma sliders drive a fullscreen gamma ramp that the compositor ignores while
// windowed, so they stop having an effect.

static bool bBorderless = false;
static HWND hGameWindow = nullptr;

// The game confined the pointer by owning the display and imports no ClipCursor at all, so a
// borderless window lets it walk onto a second monitor mid-firefight. Clipped while the window holds
// focus and released the moment it doesn't, on its own thread because Dunia never exposes its window
// procedure - there is no SetWindowLongA import to subclass through.
static void ClipCursorThread()
{
    bool bClipped = false;

    while (true)
    {
        Sleep(250);

        const bool bWantClip = bBorderless && hGameWindow && GetForegroundWindow() == hGameWindow;

        if (bWantClip)
        {
            RECT client{};
            POINT origin{};
            if (GetClientRect(hGameWindow, &client) && ClientToScreen(hGameWindow, &origin))
            {
                OffsetRect(&client, origin.x, origin.y);
                ClipCursor(&client);
                bClipped = true;
            }
        }
        else if (bClipped)
        {
            ClipCursor(nullptr);
            bClipped = false;
        }
    }
}

class Borderless
{
public:
    Borderless()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // TEST EAX,EAX / MOV EAX,[ESP+0x450] / SETNE CL - the strstr result for "-borderless".
            // MOV CL,1 in its place takes the shipped popup path unconditionally.
            auto pattern = dunia_pattern("85 C0 8B 84 24 50 04 00 00 0F 95 C1 3B C3 88 4C 24 30");
            if (pattern.empty())
                return;

            static raw_mem fnBorderlessStyle(pattern.get_first(9), { 0xB1, 0x01, 0x90 });

            // XOR EAX,EAX / CMP byte [EBP+8],AL, where EBP is the device config and +8 is its
            // fullscreen byte. Clearing it before the compare lets the game compute Windowed,
            // FullScreen_RefreshRateInHz and PresentationInterval itself, all consistently.
            pattern = dunia_pattern("8B CE E8 ? ? ? ? 33 C0 38 45 08 0F 94 C0 A3 ? ? ? ?");
            if (pattern.empty())
                return;

            static auto PresentParamsHook = safetyhook::create_mid(pattern.get_first(7), [](SafetyHookContext& regs)
            {
                if (bBorderless)
                    *(uint8_t*)(regs.ebp + 8) = 0;
            });

            // The window is created at 640x480 and sized once during engine init by this helper,
            // SetWindowMode(hWnd, title, x, y, width, height, ...), which is where the resolution and
            // the centred position arrive. Overriding its arguments is enough to cover the display,
            // and it is also the only place the window handle is available without reading a global.
            pattern = dunia_pattern("83 EC 10 8B 44 24 18 56 8B 74 24 18 57 50 56 FF 15 ? ? ? ? 8B 4C 24 2C");
            if (pattern.empty())
                return;

            static auto SetWindowModeHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
            {
                hGameWindow = *(HWND*)(regs.esp + 0x04);

                if (!bBorderless)
                    return;

                *(int32_t*)(regs.esp + 0x0C) = 0;                             // x
                *(int32_t*)(regs.esp + 0x10) = 0;                             // y
                *(int32_t*)(regs.esp + 0x14) = GetSystemMetrics(SM_CXSCREEN); // width
                *(int32_t*)(regs.esp + 0x18) = GetSystemMetrics(SM_CYSCREEN); // height
            });

            static auto BorderlessCB = []()
            {
                bBorderless = JackalFixSettings.GetInt(PREF_BORDERLESS) != 0;

                if (bBorderless)
                    fnBorderlessStyle.Write();
                else
                    fnBorderlessStyle.Restore();
            };

            BorderlessCB();

            // All three sites are read while the engine starts up, so an ini change lands on the next
            // launch rather than the current one. The cursor clip does follow it live.
            JackalFix::onIniFileChange() += []()
            {
                BorderlessCB();
            };

            JackalFix::onShutdownEvent() += []()
            {
                ClipCursor(nullptr);
            };

            CreateThreadAutoClose(nullptr, 0, [](LPVOID) -> DWORD
            {
                ClipCursorThread();
                return 0;
            }, nullptr, 0, nullptr);
        };
    }
} Borderless;
