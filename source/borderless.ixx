module;

#include <common.hxx>
#include <d3d9.h>

export module borderless;

import common;
import dunia;
import settings;

// Fullscreen, borderless and windowed, selected by DisplayMode. Dunia has all three and never
// exposes the choice. The window style comes from an undocumented "-borderless" command line
// switch with no entry in the video options; everything else hangs off one flag InitDuniaEngine
// derives at 0x10005367 from the render config's Fullscreen property. FUN_10402210 registers that
// property at offset 0x28 and Maximized at 0x2C.
//
// The flag feeds four things:
//
//     window position   saved WindowPos X/Y are read only when neither Fullscreen nor Maximized
//                       is set, so a fullscreen profile leaves the window at 0,0
//     cursor            SetWindowMode's seventh argument; non-zero means SetCursor(NULL)
//     resolution path   0x10F92040, choosing GetClientRect or a walk of EnumDisplaySettings for
//                       the nearest mode at or below the request
//     device            field +0x08 of the engine-init params at 0x10F92038, whose first 0x2C
//                       bytes become the renderer's device config, and from which
//                       D3DPRESENT_PARAMETERS.Windowed, FullScreen_RefreshRateInHz and
//                       PresentationInterval all follow
//
// It is forced twice. Engine init covers the window and the boot resolution. A second write inside
// SetResolution is needed because the render manager recomputes the device config's copy from live
// device state on every settings change: renderer vtable +0x134 is a GetPresentParameters call
// returning Windowed == 0. That write lands before SetResolution's three-branch block reads the
// byte. Clearing it after the branch, as the old Borderless option did, left a windowed device
// carrying the fullscreen branch's aspect ratio.
//
// Nothing here touches the saved video options, so the Fullscreen entry still shows what the player
// saved and no longer decides anything.
//
// Windowed needs two further repairs and borderless one, each documented at its own hook, along
// with a stock crash on applying a resolution from the main menu.

// D3DPRESENT_PARAMETERS, located through the only pair of instructions that write
// BackBufferWidth/Height. ASLR moves the block, so it cannot be addressed directly.
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
// the flag reads as zero without patching the branch or the player's setting.
static uint32_t nZeroedConfig[16] = {};
static void* pZeroedSettingsManager[4] = { nullptr, nullptr, nZeroedConfig, nullptr };

// Stands in for the video options page once the engine has destroyed it. __stdcall so the callee
// cleans the one stack argument, matching the RET 4 the call site expects.
static void __stdcall DiscardSecondDestroy(int) {}

static void* pDiscardVTable[8] = {};
static void* pDestroyedPage[1] = {};

// Dunia's classes keep their vtables in Dunia's image. Freed memory comes back zeroed or carrying
// allocator bookkeeping, and both fall outside it.
static uintptr_t nDuniaBase = 0;
static uintptr_t nDuniaEnd = 0;

static bool IsDuniaPointer(uintptr_t nAddress)
{
    return nDuniaEnd != 0 && nAddress >= nDuniaBase && nAddress < nDuniaEnd;
}

// The game confined the pointer by owning the display and imports no ClipCursor, so a borderless
// window lets it walk onto a second monitor mid-firefight. On its own thread because Dunia never
// exposes its window procedure and imports no SetWindowLongA to subclass through. Fullscreen does
// not need it and windowed should not have it.
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
            // argument. That argument picks the style: zero gives WS_CAPTION | WS_SYSMENU |
            // WS_MINIMIZEBOX, non-zero gives WS_POPUP, computed as (-(arg != 0) & 0x7F360000) +
            // 0xCA0000. Overwriting the test result rather than patching the SETNZ keeps the switch
            // working both ways, so windowed and fullscreen get a title bar back even on a command
            // line that asked for neither.
            auto borderlessSwitch = dunia_pattern("85 C0 8B 84 24 50 04 00 00 0F 95 C1 3B C3 88 4C 24 30");
            if (borderlessSwitch.empty())
                return;

            // One instruction after the SETNZ that derives the mode flag and one before the store
            // everything downstream reads. EAX holds the render config, ECX's low byte the flag.
            auto modeFlag = dunia_pattern("39 58 28 89 5C 24 78 0F 95 C1 39 58 2C 88 4C 24 30 0F 95 C0 3A CB");
            if (modeFlag.empty())
                return;

            // SetWindowMode(hWnd, title, x, y, width, height, hideCursor, maximized). The window
            // is created at 640x480 and sized once here during engine init. Also the only place the
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
                // Borderless sets it alongside fullscreen. Here the flag only picks the boot
                // resolution path, and borderless wants fullscreen's: walk the adapter's modes for
                // the nearest at or below the saved resolution. Left clear, the windowed path
                // measures the window, which covers the whole display, so the saved resolution
                // would be discarded every launch. The config byte forced inside SetResolution is
                // what makes the device windowed.
                regs.ecx = (regs.ecx & ~0xFFu) | ((IsFullscreen() || IsBorderless()) ? 1u : 0u);
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

                // Set by hand because the mode flag drives both this and the resolution path.
                *(int32_t*)(regs.esp + 0x1C) = 1;
            });

            static auto DeviceConfigHook = safetyhook::create_mid(deviceConfig.get_first(), [](SafetyHookContext& regs)
            {
                // The device config arrives by pointer in SetResolution's fourth stack argument.
                // The relocated MOV loads it into EBP a moment from now.
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

                    // ESI is the renderer. Reset resizes the window so the client area matches the
                    // backbuffer, gated on renderer+0xA4, written from the device config at
                    // 0x104250B1 and read at 0x10422676 and nowhere else. Windowed wants that;
                    // borderless must not have it, or picking a resolution below the desktop leaves
                    // the game in a small box in the corner. Cleared on every pass because the reset
                    // that does the damage comes after a resolution change rather than at startup.
                    if (IsBorderless())
                        *reinterpret_cast<uint8_t*>(regs.esi + 0xA4) = 0;
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

            // Applying a resolution from the main menu destroys the video options page twice and
            // takes the process with it. It reproduces on an untouched install, so it has nothing to
            // do with DisplayMode, but it lands on anyone who changes a resolution.
            //
            // FUN_10AD3FB0 tears the page down: it removes each child, calls FUN_10A995A0 to detach
            // the page from its parent's list, then destroys the page itself.
            //
            //   10AD4089  CALL 0x10A995A0      ; detach from the parent's list
            //   10AD408E  TEST EBP,EBP         ; the page
            //   10AD4090  JZ   0x10AD409E
            //   10AD4092  MOV  EAX,[EBP]       ; its vtable
            //   10AD4095  MOV  EDX,[EAX+0xC]
            //   10AD409C  CALL EDX             ; ...with the delete flag set
            //
            // By then the page is gone, so 0x10AD4092 reads a vtable pointer out of reclaimed memory
            // and 0x10AD4095 faults reading 0x0000000C, offset 0xC of a null vtable.
            //
            // The removal loop is what kills it, not the detach. FUN_10A995A0 only destroys the page
            // when it finds it in the parent's vector, and a dump taken with this hook installed had
            // it returning null while the page was dead anyway. The last RemoveChild takes the page
            // down with its final child, and the loop survives only because it re-reads the vtable
            // on every iteration.
            //
            // Recognising the corpse took three dumps, because freed memory does not look the same
            // twice. Two had the vtable pointer zeroed and died dereferencing it. A third, with an
            // equality test against zero already in place, had the heap address 0x15E202A4 sitting
            // there and died one instruction later calling through a null slot read out of it. So
            // the test has to be validity: a live page's vtable is inside Dunia's image and so is
            // the function in it.
            //
            // Substituting an object whose vtable slot is a no-op leaves the pause menu path, where
            // the page really is still alive, destroying it as before. Skipping the call instead is
            // not available, because the branch that could skip it is two instructions back.
            auto doubleDestroy = dunia_pattern("8B 4C 24 38 55 E8 ? ? ? ? 85 ED 74 0C 8B 45 00 8B 50 0C 6A 01 8B CD FF D2");
            if (!doubleDestroy.empty() && hDunia)
            {
                const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hDunia);
                const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                    reinterpret_cast<const uint8_t*>(hDunia) + pDos->e_lfanew);

                nDuniaBase = reinterpret_cast<uintptr_t>(hDunia);
                nDuniaEnd = nDuniaBase + pNt->OptionalHeader.SizeOfImage;

                pDiscardVTable[3] = reinterpret_cast<void*>(&DiscardSecondDestroy);  // slot 0xC
                pDestroyedPage[0] = pDiscardVTable;

                static auto DoubleDestroyHook = safetyhook::create_mid(doubleDestroy.get_first(0x0E), [](SafetyHookContext& regs)
                {
                    if (regs.ebp == 0)
                        return;

                    // EAX is still FUN_10A995A0's return value: the page when it destroyed it, null
                    // otherwise. Non-zero settles it without touching the page at all.
                    bool bAlreadyDestroyed = regs.eax != 0;

                    if (!bAlreadyDestroyed)
                    {
                        // Safe in this order. The engine reads the vtable itself two instructions
                        // from here, and the slot is only read once the vtable points into Dunia.
                        const auto nVTable = *reinterpret_cast<uintptr_t*>(regs.ebp);

                        bAlreadyDestroyed = !IsDuniaPointer(nVTable) ||
                            !IsDuniaPointer(*reinterpret_cast<uintptr_t*>(nVTable + 0xC));
                    }

                    if (bAlreadyDestroyed)
                        regs.ebp = reinterpret_cast<uintptr_t>(pDestroyedPage);
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
