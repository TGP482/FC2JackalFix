module;

#include <common.hxx>
#include <d3d9.h>

export module borderless;

import common;
import dunia;
import settings;

// Fullscreen, borderless and windowed, chosen by DisplayMode and driven entirely through the
// engine's own code paths rather than around them.
//
// Dunia already has all three. It just never exposes the choice coherently: the window style comes
// from an undocumented "-borderless" command-line switch with no entry in the video options, and
// everything else - which resolution path runs, whether the D3D9 device goes exclusive, whether the
// cursor is hidden - hangs off a single derived flag that the video options own.
//
// That flag is the whole mechanism. InitDuniaEngine computes it once at 0x10005367 from the render
// config's Fullscreen property - registered under that name in FUN_10402210, which writes offset
// 0x28 for "Fullscreen" and 0x2C for "Maximized" - and from there it fans out to:
//
//   the window position   the saved WindowPos X/Y are only read when neither Fullscreen nor
//                         Maximized is set, so a fullscreen profile leaves the window at 0,0
//   the cursor            SetWindowMode's seventh argument; non-zero means SetCursor(NULL)
//   the resolution path   `0x10F92040`, which picks between GetClientRect and walking
//                         EnumDisplaySettings for the nearest mode <= the request
//   the device            the same byte is field +0x08 of the engine-init params at 0x10F92038,
//                         whose first 0x2C bytes become the renderer's device config - and
//                         D3DPRESENT_PARAMETERS.Windowed, FullScreen_RefreshRateInHz and
//                         PresentationInterval are all derived from it
//
// So one write, early enough, moves all four together. That is what this module does: it overwrites
// the flag at the point it is produced, and lets the engine do the rest. Nothing here reimplements
// a mode.
//
// Three things need saying about the details.
//
// **The flag is forced twice, in two different places, and both are necessary.** The one at engine
// init drives the window and the boot-time resolution choice. The device config is a *copy* of
// those params taken at device creation, and the render manager recomputes its fullscreen byte from
// the live device state whenever the video options change - renderer vtable +0x134 is a
// GetPresentParameters call returning `Windowed == 0` - so without a second write inside
// SetResolution the mode would drift the first time the player touched the video options. The
// second write lands before SetResolution's three-branch block reads the byte, which also keeps the
// aspect ratio coherent: the fullscreen branch takes the config's stored aspect and the windowed
// branches derive it from the resolution, so clearing the byte after the branch - as the old
// borderless option did - produced a windowed device carrying a fullscreen aspect.
//
// **Borderless has to defeat Maximized as well.** It is a real registered property, not a dead
// field, and the last thing InitDuniaEngine does is ShowWindow(SW_MAXIMIZE) when it is set. On a
// WS_POPUP window sized to cover the display, maximising snaps it to the work area and the taskbar
// reappears over the game.
//
// **None of this writes the user's saved video options.** Every write is to a derived flag or to
// the renderer's per-run copy, so turning DisplayMode off restores stock behaviour with nothing
// left behind. The consequence is that the Fullscreen entry in the video options still shows what
// the player saved and no longer decides anything - DisplayMode outranks it.
//
// Two more things, both of which are why nobody ever used windowed mode.
//
// **The engine asks for a device reset every time it is asked whether one is needed, forever.**
// FUN_1033C7E0 decides that. It calls GetClientRect and compares the result against the render
// config's resolution at 0x1033C933; if they differ it runs AdjustWindowRectEx over the *config*
// resolution and, for a window that is not WS_POPUP, compares the result against SM_CXFULLSCREEN /
// SM_CYFULLSCREEN at 0x1033C9F9. Bigger than either and it returns "device change needed".
//
// Ask for a resolution equal to the desktop - the obvious thing to do - and a bordered window can
// never satisfy that, because the frame alone puts it over. The client rect can then never equal
// the config resolution either, so the test is reached on every call and answers the same way every
// time. Nothing converges: the clamp it computes goes into renderMgr+0x330, which the function
// itself zeroes on entry, and the comparison is always against the unclamped config. The result is
// a release/Reset/restore cycle several times a second, forever, which is what the low erratic
// frame rate actually was.
//
// Fullscreen returns before reaching it. Borderless takes the WS_POPUP branch, where the adjusted
// size equals the requested size because a popup has no frame, so it passes. A bordered window is
// the only shape that trips it. Windowed therefore zeroes the two adjusted extents just before the
// comparison, which is the smallest edit that makes the answer "no change needed" without touching
// the clamp, the config, or the other reasons the function can legitimately ask for a reset.
//
// **And the presentation parameters are wrong for a composed window.** SetResolution hardcodes
// PresentationInterval to D3DPRESENT_INTERVAL_IMMEDIATE for any windowed device - 0x104235CF,
// unconditional, the config's own vsync value only ever reaches the fullscreen branch - and leaves
// BackBufferCount at zero, which D3D9 reads as one.
//
// Exclusive fullscreen does not care: it flips. Borderless does not care either, because a popup
// covering the whole display gets an independent flip out of the compositor and never touches the
// redirection surface. A bordered window is again the one case that takes the composed blit path,
// and there those two values are the worst possible pair. IMMEDIATE buys nothing - a composed
// window cannot tear, the compositor owns the vblank - so all it does is let the game run ahead and
// then block inside Present. With a single backbuffer there is nothing to absorb the jitter. So
// windowed asks for INTERVAL_ONE and two backbuffers instead; the ceiling was the refresh rate
// either way.

// D3DPRESENT_PARAMETERS. Reached through the only pair of instructions that write
// BackBufferWidth/Height rather than by address, because ASLR moves it.
static D3DPRESENT_PARAMETERS* pPresentParams = nullptr;

enum DisplayModeSetting
{
    DISPLAY_FULLSCREEN = 1,
    DISPLAY_BORDERLESS = 2,
    DISPLAY_WINDOWED = 3,
};

static int32_t nDisplayMode = DISPLAY_BORDERLESS;
static HWND hGameWindow = nullptr;

static bool IsFullscreen() { return nDisplayMode == DISPLAY_FULLSCREEN; }
static bool IsBorderless() { return nDisplayMode == DISPLAY_BORDERLESS; }

// Stands in for the settings manager at the one site that reads Maximized straight out of it.
// Reading the flag out of a block of zeroes is cheaper and less invasive than patching the branch,
// and it leaves the player's actual setting untouched.
static uint32_t nZeroedConfig[16] = {};
static void* pZeroedSettingsManager[4] = { nullptr, nullptr, nZeroedConfig, nullptr };

// The game confined the pointer by owning the display and imports no ClipCursor at all, so a
// borderless window lets it walk onto a second monitor mid-firefight. Clipped while the window
// holds focus and released the moment it doesn't, on its own thread because Dunia never exposes its
// window procedure - there is no SetWindowLongA import to subclass through. Fullscreen needs none
// of this, the display mode change does it; windowed wants none of it, the pointer belongs to the
// desktop.
static void ClipCursorThread()
{
    bool bClipped = false;

    while (true)
    {
        Sleep(250);

        const bool bWantClip = IsBorderless() && hGameWindow && GetForegroundWindow() == hGameWindow;

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
            // The strstr result for "-borderless", on its way to the window creator's ninth
            // argument. That argument picks the style outright: zero gives
            // WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, non-zero gives WS_POPUP, computed as
            // (-(arg != 0) & 0x7F360000) + 0xCA0000. Overwriting the result of the test rather than
            // patching the SETNZ keeps the switch working in both directions, so windowed and
            // fullscreen get a real title bar back even on a command line that asked for neither.
            auto borderlessSwitch = dunia_pattern("85 C0 8B 84 24 50 04 00 00 0F 95 C1 3B C3 88 4C 24 30");
            if (borderlessSwitch.empty())
                return;

            // Where the mode flag is born, one instruction after the SETNZ that derives it from the
            // render config's Fullscreen property and one before the store that everything
            // downstream reads. EAX holds the render config here, ECX's low byte holds the flag.
            auto modeFlag = dunia_pattern("39 58 28 89 5C 24 78 0F 95 C1 39 58 2C 88 4C 24 30 0F 95 C0 3A CB");
            if (modeFlag.empty())
                return;

            // The window is created at 640x480 and sized once during engine init by this helper,
            // SetWindowMode(hWnd, title, x, y, width, height, hideCursor, maximized). Overriding
            // its arguments is enough to cover the display, and it is also the only place the
            // window handle is available without reading a global.
            auto setWindowMode = dunia_pattern("83 EC 10 8B 44 24 18 56 8B 74 24 18 57 50 56 FF 15 ? ? ? ?");
            if (setWindowMode.empty())
                return;

            // SetResolution loading its device config, one instruction before the three-branch
            // block reads the fullscreen byte out of it.
            auto deviceConfig = dunia_pattern("8B AC 24 64 02 00 00 80 7D 08 00 74 24 8B 8C 24 5C 02 00 00");
            if (deviceConfig.empty())
                return;

            static auto BorderlessSwitchHook = safetyhook::create_mid(borderlessSwitch.get_first(), [](SafetyHookContext& regs)
            {
                // The relocated TEST EAX,EAX runs on this value and the SETNZ reads its flags.
                regs.eax = IsBorderless() ? 1 : 0;
            });

            static auto ModeFlagHook = safetyhook::create_mid(modeFlag.get_first(0x0D), [](SafetyHookContext& regs)
            {
                regs.ecx = (regs.ecx & ~0xFFu) | (IsFullscreen() ? 1u : 0u);
            });

            static auto SetWindowModeHook = safetyhook::create_mid(setWindowMode.get_first(), [](SafetyHookContext& regs)
            {
                hGameWindow = *(HWND*)(regs.esp + 0x04);

                if (!IsBorderless())
                    return;

                *(int32_t*)(regs.esp + 0x0C) = 0;                             // x
                *(int32_t*)(regs.esp + 0x10) = 0;                             // y
                *(int32_t*)(regs.esp + 0x14) = GetSystemMetrics(SM_CXSCREEN); // width
                *(int32_t*)(regs.esp + 0x18) = GetSystemMetrics(SM_CYSCREEN); // height

                // The cursor argument would otherwise follow the mode flag, which borderless has
                // just cleared to get the windowed resolution path. Borderless should still feel
                // like fullscreen, so it is set back by hand rather than by leaving the flag on and
                // taking the mode-enumeration path with it.
                *(int32_t*)(regs.esp + 0x1C) = 1;
            });

            static auto DeviceConfigHook = safetyhook::create_mid(deviceConfig.get_first(), [](SafetyHookContext& regs)
            {
                // The device config arrives by pointer in SetResolution's fourth stack argument;
                // the relocated MOV loads it into EBP a moment from now.
                if (auto* pConfig = *reinterpret_cast<uint8_t**>(regs.esp + 0x264))
                    pConfig[8] = IsFullscreen() ? 1 : 0;
            });

            // FUN_1033C7E0, immediately after AdjustWindowRectEx and the two GetSystemMetrics
            // calls, with the adjusted window extents live in EDI and EBP and the comparison that
            // decides "device change needed" six instructions away. Chosen over the comparison
            // itself because these six bytes are one whole instruction with no branch in them.
            auto resizeDemand = dunia_pattern("2B 7C 24 28 2B 6C 24 2C 89 44 24 18 8B 01 8B 90 34 01 00 00 FF D2 84 C0 75 75 81 E3 00 00 00 80");
            if (!resizeDemand.empty())
            {
                static auto ResizeDemandHook = safetyhook::create_mid(resizeDemand.get_first(0x1A), [](SafetyHookContext& regs)
                {
                    // Only a bordered window reaches the comparison in a state where it can never
                    // pass. Zero extents make both halves of it succeed, so the function falls
                    // through to its ordinary "nothing to do" return instead of demanding a reset
                    // it will demand again a frame later. Both registers are dead afterwards - the
                    // epilogue only pops them.
                    if (nDisplayMode == DISPLAY_WINDOWED)
                    {
                        regs.edi = 0;
                        regs.ebp = 0;
                    }
                });
            }

            // Both of the values windowed needs are in the present parameters, and the block is
            // only ever addressed absolutely, so its base comes from the operand of the one pair of
            // instructions that write BackBufferWidth and BackBufferHeight.
            auto presentParams = dunia_pattern("8B 56 1C 89 15 ? ? ? ? 8B 46 20 A3 ? ? ? ? 8A 4D 08 F6 D9");
            auto presentTail = dunia_pattern("8B 94 24 7C 02 00 00 8B 84 24 78 02 00 00 8B 8C 24 74 02 00 00");

            if (!presentParams.empty() && !presentTail.empty())
            {
                pPresentParams = reinterpret_cast<D3DPRESENT_PARAMETERS*>(*presentParams.get_first<uint32_t*>(5));

                // The first instruction after SetResolution has finished with the present
                // parameters and before it hands them to the multisample check. Everything the
                // engine intended to write is in place; this only overrides what it had no way to
                // get right, and it runs again on every resolution change rather than once.
                static auto PresentTailHook = safetyhook::create_mid(presentTail.get_first(), [](SafetyHookContext& regs)
                {
                    if (!pPresentParams)
                        return;

                    // EBP is still the device config here.
                    const bool bWindowedDevice = *reinterpret_cast<uint8_t*>(regs.ebp + 8) == 0;

                    if (nDisplayMode == DISPLAY_WINDOWED && bWindowedDevice)
                    {
                        pPresentParams->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
                        pPresentParams->BackBufferCount = 2;
                    }
                });
            }

            // The last thing InitDuniaEngine does, and the one place Maximized is read straight out
            // of the settings manager rather than from the flag above. Only borderless cares:
            // maximising a popup window that already covers the display snaps it to the work area
            // and lets the taskbar back on top.
            auto maximized = dunia_pattern("8B 51 08 39 5A 2C 74 0A A1 ? ? ? ? 6A 03 50 EB 09");
            if (!maximized.empty())
            {
                static auto MaximizedHook = safetyhook::create_mid(maximized.get_first(), [](SafetyHookContext& regs)
                {
                    if (IsBorderless())
                        regs.ecx = reinterpret_cast<uintptr_t>(pZeroedSettingsManager);
                });
            }

            static auto DisplayModeCB = []()
            {
                nDisplayMode = JackalFixSettings.GetInt(PREF_DISPLAYMODE);
            };

            DisplayModeCB();

            // The window style, its rect and the boot resolution path are all read while the engine
            // starts up, so an ini change lands on the next launch rather than the current one. The
            // device's fullscreen byte and the cursor clip do follow it live.
            JackalFix::onIniFileChange() += []()
            {
                DisplayModeCB();
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
