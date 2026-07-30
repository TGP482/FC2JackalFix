module;

#include <common.hxx>
#include <d3d9.h>

export module borderless;

import common;
import dunia;
import settings;

// Fullscreen, borderless and windowed, chosen by DisplayMode. Dunia has all three already: the
// window style comes from an undocumented "-borderless" command-line switch with no entry in the
// video options, and everything else hangs off one derived flag the video options own.
//
// InitDuniaEngine computes that flag at 0x10005367 from the render config's Fullscreen property
// (FUN_10402210 registers offset 0x28 Fullscreen, 0x2C Maximized). It picks the window position
// (the saved WindowPos X/Y are only read when neither Fullscreen nor Maximized is set, so a
// fullscreen profile leaves the window at 0,0), the cursor, the resolution path at 0x10F92040
// (GetClientRect vs EnumDisplaySettings for the nearest mode <= the request), and the device: the
// same byte is +0x08 of the engine-init params at 0x10F92038, whose first 0x2C bytes become the
// renderer's device config, source of Windowed, FullScreen_RefreshRateInHz and
// PresentationInterval.
//
// Forced twice. The render manager recomputes its fullscreen byte from live device state on every
// video options change (renderer vtable +0x134, GetPresentParameters, Windowed == 0), so
// SetResolution needs its own write. That write must land before SetResolution's three-branch block
// reads the byte: the fullscreen branch takes the config's stored aspect and the windowed branches
// derive it from the resolution, so clearing the byte after the branch, as the old borderless
// option did, produces a windowed device carrying a fullscreen aspect. Nothing is persisted, so
// DisplayMode outranks the saved Fullscreen entry.
//
// FUN_1033C7E0 demands a device reset forever on a bordered window. It compares GetClientRect
// against the config resolution at 0x1033C933, then runs AdjustWindowRectEx over the config
// resolution and, for a non-WS_POPUP window, compares against SM_CXFULLSCREEN / SM_CYFULLSCREEN at
// 0x1033C9F9; bigger than either returns "device change needed". At desktop resolution the frame
// alone puts a bordered window over and nothing converges, since the clamp goes to renderMgr+0x330,
// which the function zeroes on entry, and the comparison is always against the unclamped config.
// The symptom is a release/Reset/restore cycle several times a second, forever, which is what the
// low erratic frame rate actually was. Fullscreen returns earlier and a popup has no frame, so only
// windowed needs the fix.
//
// SetResolution also hardcodes PresentationInterval to D3DPRESENT_INTERVAL_IMMEDIATE for any
// windowed device (0x104235CF; the config's vsync only reaches the fullscreen branch) and leaves
// BackBufferCount at zero, which D3D9 reads as one. Exclusive fullscreen flips, and a popup
// covering the whole display gets an independent flip out of the compositor and never touches the
// redirection surface, so neither cares. A bordered window takes the composed blit path, where it
// cannot tear because the compositor owns the vblank: IMMEDIATE buys nothing and only lets the game
// run ahead and then block inside Present, with one backbuffer to absorb the jitter. Windowed asks
// for INTERVAL_ONE and two backbuffers; the ceiling was the refresh rate either way.

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

// Stands in for the settings manager at the one site that reads Maximized straight out of it, so
// the player's saved setting is left alone.
static uint32_t nZeroedConfig[16] = {};
static void* pZeroedSettingsManager[4] = { nullptr, nullptr, nZeroedConfig, nullptr };

// The game imports no ClipCursor, so a borderless window lets the pointer walk onto a second
// monitor. Clipped while the window holds focus and released the moment it loses it. Fullscreen
// needs none of this, the display mode change does it; windowed wants none of it, the pointer
// belongs to the desktop. Own thread because Dunia never exposes its window procedure and imports
// no SetWindowLongA to subclass through.
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
            // The strstr result for "-borderless", feeding the window creator's ninth argument:
            // zero gives WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, non-zero gives WS_POPUP, as
            // (-(arg != 0) & 0x7F360000) + 0xCA0000. Overwriting the tested value rather than the
            // SETNZ keeps the switch working both ways.
            auto borderlessSwitch = dunia_pattern("85 C0 8B 84 24 50 04 00 00 0F 95 C1 3B C3 88 4C 24 30");
            if (borderlessSwitch.empty())
                return;

            // The mode flag, one instruction after the SETNZ that derives it and one before the
            // store everything downstream reads. EAX holds the render config, ECX's low byte the
            // flag.
            auto modeFlag = dunia_pattern("39 58 28 89 5C 24 78 0F 95 C1 39 58 2C 88 4C 24 30 0F 95 C0 3A CB");
            if (modeFlag.empty())
                return;

            // SetWindowMode(hWnd, title, x, y, width, height, hideCursor, maximized), which sizes
            // the 640x480 window once during engine init. Also the only place the window handle is
            // available without reading a global.
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

                // Borderless clears the mode flag to get the windowed resolution path, which also
                // re-enables the saved WindowPos read, so x and y are pinned to 0 here.
                *(int32_t*)(regs.esp + 0x0C) = 0;                             // x
                *(int32_t*)(regs.esp + 0x10) = 0;                             // y
                *(int32_t*)(regs.esp + 0x14) = GetSystemMetrics(SM_CXSCREEN); // width
                *(int32_t*)(regs.esp + 0x18) = GetSystemMetrics(SM_CYSCREEN); // height

                // The cursor argument follows the mode flag too: non-zero means SetCursor(NULL),
                // so borderless sets it back by hand.
                *(int32_t*)(regs.esp + 0x1C) = 1;
            });

            static auto DeviceConfigHook = safetyhook::create_mid(deviceConfig.get_first(), [](SafetyHookContext& regs)
            {
                // The device config is SetResolution's fourth stack argument; the relocated MOV
                // loads it into EBP a moment from now.
                if (auto* pConfig = *reinterpret_cast<uint8_t**>(regs.esp + 0x264))
                    pConfig[8] = IsFullscreen() ? 1 : 0;
            });

            // FUN_1033C7E0 after AdjustWindowRectEx and the two GetSystemMetrics calls: adjusted
            // extents live in EDI and EBP, comparison six instructions away. Anchored here rather
            // than on the comparison because these six bytes are one instruction with no branch.
            auto resizeDemand = dunia_pattern("2B 7C 24 28 2B 6C 24 2C 89 44 24 18 8B 01 8B 90 34 01 00 00 FF D2 84 C0 75 75 81 E3 00 00 00 80");
            if (!resizeDemand.empty())
            {
                static auto ResizeDemandHook = safetyhook::create_mid(resizeDemand.get_first(0x1A), [](SafetyHookContext& regs)
                {
                    // Zero is under both metrics, so neither half of the comparison can report a
                    // change and the function falls through to its "nothing to do" return. Both
                    // registers are dead afterwards; the epilogue only pops them.
                    if (nDisplayMode == DISPLAY_WINDOWED)
                    {
                        regs.edi = 0;
                        regs.ebp = 0;
                    }
                });
            }

            // The present parameters block is only ever addressed absolutely, so its base comes
            // from the operand of the one pair of instructions that write BackBufferWidth/Height.
            auto presentParams = dunia_pattern("8B 56 1C 89 15 ? ? ? ? 8B 46 20 A3 ? ? ? ? 8A 4D 08 F6 D9");
            auto presentTail = dunia_pattern("8B 94 24 7C 02 00 00 8B 84 24 78 02 00 00 8B 8C 24 74 02 00 00");

            if (!presentParams.empty() && !presentTail.empty())
            {
                pPresentParams = reinterpret_cast<D3DPRESENT_PARAMETERS*>(*presentParams.get_first<uint32_t*>(5));

                // First instruction after SetResolution has finished with the present parameters
                // and before it hands them to the multisample check, so this runs on every
                // resolution change.
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

            // Maximized is a registered property, and the last thing InitDuniaEngine does is
            // ShowWindow(SW_MAXIMIZE) when it is set. This is the one place it is read straight out
            // of the settings manager. Only borderless cares: maximising a popup that already
            // covers the display snaps it to the work area and lets the taskbar back on top.
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

            // The window style, its rect and the boot resolution path are read during engine
            // startup, so an ini change lands on the next launch. The device's fullscreen byte and
            // the cursor clip do follow it live.
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
