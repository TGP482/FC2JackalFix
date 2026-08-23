module;

#include <common.hxx>
#include <d3d9.h>
#include <intrin.h>

export module borderless;

import common;
import dunia;
import settings;

// Fullscreen, borderless and windowed, chosen by DisplayMode through the engine's own code paths.
//
// The window style comes from an undocumented "-borderless" command-line switch. Everything else
// hangs off one derived flag, computed by InitDuniaEngine at 0x10005367 from the render config's
// Fullscreen property (FUN_10402210 writes offset 0x28 for "Fullscreen", 0x2C for "Maximized").
// It fans out to:
//
//   window position   saved WindowPos X/Y are only read when neither Fullscreen nor Maximized is
//                     set, so a fullscreen profile leaves the window at 0,0
//   cursor            SetWindowMode's seventh argument; non-zero means SetCursor(NULL)
//   resolution path   0x10F92040, GetClientRect or an EnumDisplaySettings walk for the nearest
//                     mode <= the request
//   device            field +0x08 of the engine-init params at 0x10F92038, whose first 0x2C bytes
//                     become the renderer's device config, source of
//                     D3DPRESENT_PARAMETERS.Windowed, FullScreen_RefreshRateInHz and
//                     PresentationInterval
//
// The flag is overwritten where it is produced, twice: engine init drives the window and the boot
// resolution, and the render manager recomputes the device config's copy from the live device on
// every video options change (renderer vtable +0x134 is a GetPresentParameters returning
// Windowed == 0). The second write goes in before SetResolution's three-branch block reads the
// byte, not after: the fullscreen branch takes the config's stored aspect while the windowed
// branches derive it from the resolution, so clearing it afterwards (as the old borderless option
// did) gave a windowed device with a fullscreen aspect. No saved video option is written.
//
// Two renderers sit behind one interface. FUN_1033C560 picks and stores either in DAT_11609668:
// D3D10 vtable 0x10E52CA8, D3D9 0x10E53158. The video manager above them is shared, so
// FUN_1033C7E0 and FUN_1034CA80 serve both; below it nothing is shared, and D3D10's Reset slot
// +0x54 is a stub returning 1 since it drives everything through DXGI. Each fault below says which
// renderer it belongs to.
//
// The crash on changing resolution from the main menu is a GUI lifetime fault and lives in
// guiduplicates.

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

// Stands in for the settings manager at the one site that reads Maximized straight out of it;
// reading the flag out of zeroes leaves the player's own setting untouched.
static uint32_t nZeroedConfig[16] = {};
static void* pZeroedSettingsManager[4] = { nullptr, nullptr, nZeroedConfig, nullptr };

// Renderer vtable+0x134, FUN_10422E50. Alt-tab out of exclusive fullscreen came back windowed one
// resolution smaller: both renderers answer from a live query and both answer "windowed" when they
// cannot answer at all (D3D9 falls to XOR AL,AL at 0x10422E9E; D3D10's FUN_1041F710 clears its
// result on a null swapchain or a bad HRESULT from GetFullscreenState), and under DXGI it really
// is windowed since MakeWindowAssociation is called with flags 4. Replaced outright: DisplayMode
// owns the mode.
static SafetyHookInline IsDeviceFullscreenHook{};

static uint8_t __fastcall IsDeviceFullscreen(void*, void*)
{
    return IsFullscreen() ? 1 : 0;
}

// D3D10 renderer fields, off the object DAT_11609668 points at. Its DXGI_SWAP_CHAIN_DESC is
// embedded at +0x4C rather than living in a global the way the D3D9 present parameters do.
static constexpr uintptr_t nRendererSwapChain = 0x40;   // IDXGISwapChain*
static constexpr uintptr_t nRendererFullscreen = 0x44;  // the cached GetFullscreenState answer
static constexpr uintptr_t nDescWidth = 0x4C;           // BufferDesc.Width
static constexpr uintptr_t nDescHeight = 0x50;
static constexpr uintptr_t nDescFormat = 0x5C;
static constexpr uintptr_t nDescBufferCount = 0x74;
static constexpr uintptr_t nDescFlags = 0x84;
static constexpr uintptr_t nRendererWindow = 0x88;      // the HWND the swapchain was made for

// IDXGISwapChain vtable slots.
static constexpr uintptr_t nSetFullscreenState = 0x28;
static constexpr uintptr_t nResizeBuffers = 0x34;
static constexpr uintptr_t nResizeTarget = 0x38;

using SetFullscreenState_t = HRESULT(__stdcall*)(void*, BOOL, void*);
using ResizeBuffers_t = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, UINT, UINT);
using RebuildViews_t = void(__thiscall*)(void*);

// FUN_1041FE80: re-acquires the backbuffer, rebuilds the render target view, resets the viewport.
// Taken from the displacement of SetMode's call to it, having no external callers and no
// distinctive prologue.
static RebuildViews_t pRebuildViews = nullptr;

// A stand-in swapchain for the one call borderless does not want made. Only vtable+0x38 is ever
// read through it: the substitution lasts exactly as long as ResizeTarget's argument list, and the
// real pointer is reloaded from renderer+0x40 four instructions later.
static HRESULT __stdcall NullResizeTarget(void*, const void*) { return S_OK; }
static void* pStubSwapChainVtbl[16] = {};
static void* pStubSwapChain[1] = { pStubSwapChainVtbl };

// Switching mode while the game runs.
//
// The window's style is decided once and never revisited: the window is created during engine init
// and the ninth argument to its creator picks between WS_POPUP and a captioned window, so a window
// born windowed keeps its title bar for the rest of the run. That is why changing the setting and
// taking a device reset left borderless looking like a plain window.
//
// The reset does not restyle it either: the engine's post-reset call is a SetWindowPos with
// SWP_NOMOVE and no SWP_FRAMECHANGED. So the restyle is done here, on that same reset, the one
// moment the device is already being rebuilt around it.
//
// The two styles are the engine's own, computed by the window creator as
// (-(borderless != 0) & 0x7F360000) + 0xCA0000, where 0CA0000h is WS_CAPTION | WS_SYSMENU |
// WS_MINIMIZEBOX and the sum for borderless is WS_POPUP.
static constexpr LONG nWindowedStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
static constexpr LONG nBorderlessStyle = WS_POPUP;
static constexpr LONG nModeStyles = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
                                  | WS_THICKFRAME | WS_POPUP;

// The mode the window is currently wearing, so a reset that changes nothing leaves it alone.
static int32_t nWindowMode = 0;

// The window the game draws into, for the parts of the mod that measure it. Null until engine init
// has sized it.
export HWND JackalFixGameWindow()
{
    return hGameWindow;
}

static void ApplyWindowMode(HWND hWnd)
{
    if (hWnd == nullptr || !IsWindow(hWnd) || IsFullscreen())
        return;

    const auto bBorderlessNow = IsBorderless();
    const auto nStyle = GetWindowLongA(hWnd, GWL_STYLE);
    const auto nWanted = (nStyle & ~nModeStyles) | (bBorderlessNow ? nBorderlessStyle : nWindowedStyle);

    SetWindowLongA(hWnd, GWL_STYLE, nWanted);

    if (bBorderlessNow)
    {
        // The whole display, as the boot path gives it.
        SetWindowPos(hWnd, nullptr, 0, 0,
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    else
    {
        // A captioned window is sized so its *client* area is the backbuffer, which is what the
        // engine's windowed path wants. Centred, because it may be arriving from a borderless
        // origin of 0,0 with a caption now sitting above it.
        const auto nWidth = pPresentParams != nullptr && pPresentParams->BackBufferWidth != 0
            ? static_cast<LONG>(pPresentParams->BackBufferWidth) : 1280;
        const auto nHeight = pPresentParams != nullptr && pPresentParams->BackBufferHeight != 0
            ? static_cast<LONG>(pPresentParams->BackBufferHeight) : 720;

        RECT frame{ 0, 0, nWidth, nHeight };
        AdjustWindowRectEx(&frame, static_cast<DWORD>(nWanted), FALSE,
                           static_cast<DWORD>(GetWindowLongA(hWnd, GWL_EXSTYLE)));

        const auto nFrameW = frame.right - frame.left;
        const auto nFrameH = frame.bottom - frame.top;
        const auto nX = (GetSystemMetrics(SM_CXSCREEN) - nFrameW) / 2;
        const auto nY = (GetSystemMetrics(SM_CYSCREEN) - nFrameH) / 2;

        SetWindowPos(hWnd, nullptr, nX > 0 ? nX : 0, nY > 0 ? nY : 0, nFrameW, nFrameH,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    nWindowMode = nDisplayMode;
}

static ULONGLONG nLastFullscreenAttempt = 0;

// Nothing calls SetMode after an alt-tab, because every check that would notice agrees with the
// windowed state DXGI left behind, so the transition has to be made from here.
static void ReenterFullscreen(uint8_t* pRenderer)
{
    auto* pSwapChain = *reinterpret_cast<void**>(pRenderer + nRendererSwapChain);
    const auto hWnd = *reinterpret_cast<HWND*>(pRenderer + nRendererWindow);

    if (!pSwapChain || !hWnd)
        return;

    // Taking the display back the moment DXGI hands it over starts a transition loop.
    if (GetForegroundWindow() != hWnd || IsIconic(hWnd))
        return;

    // Keeps a driver that refuses the transition off the frame path.
    const auto nNow = GetTickCount64();
    if (nNow - nLastFullscreenAttempt < 500)
        return;
    nLastFullscreenAttempt = nNow;

    auto** ppVtbl = *reinterpret_cast<void***>(pSwapChain);

    if (FAILED(reinterpret_cast<SetFullscreenState_t>(ppVtbl[nSetFullscreenState / 4])(pSwapChain, TRUE, nullptr)))
        return;

    // The two steps SetMode runs after its own transition. Without them the render target view
    // still refers to the buffers the swapchain had before the mode change.
    reinterpret_cast<ResizeBuffers_t>(ppVtbl[nResizeBuffers / 4])(pSwapChain,
        *reinterpret_cast<UINT*>(pRenderer + nDescBufferCount),
        *reinterpret_cast<UINT*>(pRenderer + nDescWidth),
        *reinterpret_cast<UINT*>(pRenderer + nDescHeight),
        *reinterpret_cast<UINT*>(pRenderer + nDescFormat),
        *reinterpret_cast<UINT*>(pRenderer + nDescFlags));

    if (pRebuildViews)
        pRebuildViews(pRenderer);
}

// The game confined the pointer by owning the display and imports no ClipCursor, so a borderless
// window lets it walk onto a second monitor. On its own thread because Dunia never exposes its
// window procedure and there is no SetWindowLongA import to subclass through. Fullscreen gets it
// from the mode change; windowed leaves the pointer to the desktop.
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


// Making a display setting take effect without a restart.
//
// The machinery is the engine's. CFCXOptionDisplayPage's apply raises the render settings broadcast
// at Dunia+3F8AB0; the render manager's observer on it is MOV byte ptr [ECX+30h],1 / RET. The
// per-frame device tick at Dunia+34CA80 tests that byte and calls Dunia+354B30, which asks
// FUN_1033C7E0 whether the mode differs and only on a yes invokes the functor at manager+10Ch,
// ending in SetMode on the next tick. So: set the byte, and answer that question yes once.
//
// FUN_1033C7E0 is called rather than stubbed, because it writes the size override at
// manager+330h/+334h that the mode change then uses; only its answer is overridden.
//
// It has two callers, both asking on the same tick:
//
//     10354B30   the functor at manager+10Ch, which ends in SetMode
//     10358410   Dunia+33E2B0, which rebuilds nothing
//
// A yes handed to the second is lost, which is what "the setting does nothing until a restart" was
// (not handler install order). The override is spent only on the call at 10354B3C, recognised by
// the address it returns to.

// The render manager's "the profile moved" byte: set by the broadcast observer, read by the device
// tick before it asks anything else.
static constexpr ptrdiff_t nRenderManagerDirty = 0x30;

// push 0 / push 3B8h / call <alloc> / add esp,8 / test eax,eax / jz / mov ecx,eax /
// call <render manager ctor> / mov [<render manager>],eax. The manager's only construction.
static const char* const szRenderManagerPattern =
    "6A 00 68 B8 03 00 00 E8 ? ? ? ? 83 C4 08 85 C0 74 0D 8B C8 E8 ? ? ? ? A3 ? ? ? ?";
static constexpr ptrdiff_t nRenderManagerPointer = 27; // disp32 of mov [<render manager>],eax

// The head of Dunia+354B30: loads the render profile, hands it to the comparison, tests the dirty
// byte. Both things wanted here are in that one sequence.
static const char* const szModeCheckPattern = "53 56 57 8B 3D ? ? ? ? 57 8B F1 E8 ? ? ? ? 80 7E 30 00";
static constexpr ptrdiff_t nModeCheckCall = 12;       // call <does the mode differ>

static void** ppRenderManager = nullptr;

// The instruction after the call at 10354B3C, the only return address a forced yes may be spent on.
static const void* pModeCheckReturn = nullptr;

static SafetyHookInline ModeDiffersHook{};

// Raised when one of the settings below moves, lowered once the engine has gone through with the
// mode change. Read from the engine's threads, written from the file watcher's.
static std::atomic<bool> bModeChangePending = false;

// What was in force the last time the engine built its device, so a broadcast is only asked for
// when something that matters has really moved rather than on every APPLY.
static int32_t nAppliedDisplayMode = 0;
static int32_t nAppliedBaseW = 0;
static int32_t nAppliedBaseH = 0;
static int32_t nAppliedScalingFilter = 0;

static uint8_t __fastcall ModeDiffers(void* pRenderManager, void* pEdx, void* pProfile)
{
    const auto* pReturn = _ReturnAddress();

    const auto nStock = ModeDiffersHook.fastcall<uint8_t>(pRenderManager, pEdx, pProfile);

    if (pModeCheckReturn != nullptr && pReturn != pModeCheckReturn)
        return nStock;

    const auto bPending = bModeChangePending.exchange(false);

    if (!bPending)
        return nStock;

    return 1;
}

// Asks the engine to rebuild its device at the next opportunity. Writing the byte is all this does:
// the tick that reads it belongs to the engine, and so does the thread it runs on.
static void RequestModeChange()
{
    if (ppRenderManager == nullptr)
        return;

    auto* pRenderManager = static_cast<uint8_t*>(*ppRenderManager);
    if (pRenderManager == nullptr)
        return;

    bModeChangePending = true;
    *(pRenderManager + nRenderManagerDirty) = 1;
}

// Order against the mode hooks and internalres does not matter: the rebuild is only asked for, and
// the engine gets to it on its next tick, by which time every init handler has had its turn.
static void InstallModeReapply()
{
    auto manager = dunia_pattern(szRenderManagerPattern);
    if (!manager.empty())
        ppRenderManager = *manager.get_first<void**>(nRenderManagerPointer);

    auto modeCheck = dunia_pattern(szModeCheckPattern);
    if (!modeCheck.empty())
    {
        auto* pCall = modeCheck.get_first<uint8_t>(nModeCheckCall);
        auto* pDiffers = pCall + 5 + *reinterpret_cast<int32_t*>(pCall + 1);
        pModeCheckReturn = pCall + 5;
        ModeDiffersHook = safetyhook::create_inline(pDiffers, ModeDiffers);
    }

    static auto Remember = []()
    {
        nAppliedDisplayMode = JackalFixSettings.GetInt(PREF_DISPLAYMODE);
        nAppliedBaseW = JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONX);
        nAppliedBaseH = JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONY);
        nAppliedScalingFilter = JackalFixSettings.GetInt(PREF_SCALINGFILTER);
    };

    Remember();

    JackalFix::onIniFileChange() += []()
    {
        const auto bMoved =
            nAppliedDisplayMode != JackalFixSettings.GetInt(PREF_DISPLAYMODE)
            || nAppliedBaseW != JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONX)
            || nAppliedBaseH != JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONY)
            || nAppliedScalingFilter != JackalFixSettings.GetInt(PREF_SCALINGFILTER);

        Remember();

        if (bMoved)
            RequestModeChange();
    };
}

// The engine's frame rate readout, behind the showFps console variable. The renderer's frame
// function at 0x104CFC90 tests this global and, when non-zero, formats "FPS: %.1f" from the float
// at [0x11609688]+0x164 and prints it through the debug text object at [0x10FE6D7C]. Nothing
// writes it apart from FUN_104D0510 registering it as showFps, so the setting is a single store
// and needs no hook.
//
// The console help string "0 (on) or 1 (off)" is backwards: non-zero draws.
static int32_t* pShowFps = nullptr;

// Into the pattern below, to the CMP's relocated address operand.
static constexpr uintptr_t nShowFpsOperand = 6;

static void InstallFpsCounter()
{
    // The CMP carries nothing but the relocated address, so the anchor reaches back to the FSTP and
    // forward through the two globals that follow:
    //
    //     104cfd52   FSTP dword ptr [ESP + 0xC]
    //     104cfd56   CMP  dword ptr [0x11644734],0x0
    auto pattern = dunia_pattern("D9 5C 24 0C 83 3D ? ? ? ? 00 74 ? 83 3D ? ? ? ? 00 8B 35 ? ? ? ? 75 07 33 C9");
    if (pattern.empty())
        return;

    pShowFps = *reinterpret_cast<int32_t**>(pattern.get_first(nShowFpsOperand));

    static constexpr auto ApplyFpsCounter = []()
    {
        *pShowFps = JackalFixSettings.GetInt(PREF_FPSCOUNTER) != 0;
    };

    // Written before the console registers the variable, which only reads the global to publish its
    // starting value, so the setting is what the console reports.
    ApplyFpsCounter();

    JackalFix::onIniFileChange() += ApplyFpsCounter;
}

class Borderless
{
public:
    Borderless()
    {
        // Registered ahead of the handler below because that one gives up on a missing pattern and
        // this shares none of them.
        JackalFix::onDuniaInitEvent() += []()
        {
            InstallModeReapply();
        };

        // Separate for the same reason, and it shares nothing with either of the others.
        JackalFix::onDuniaInitEvent() += []()
        {
            InstallFpsCounter();
        };

        JackalFix::onDuniaInitEvent() += []()
        {
            // The strstr result for "-borderless", on its way to the window creator's ninth
            // argument, which picks the style outright: zero gives
            // WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, non-zero gives WS_POPUP, computed as
            // (-(arg != 0) & 0x7F360000) + 0xCA0000. Overwriting the test result rather than
            // patching the SETNZ keeps the switch working in both directions, so windowed and
            // fullscreen get a title bar back even on a command line that asked for neither.

            auto borderlessSwitch = dunia_pattern("85 C0 8B 84 24 50 04 00 00 0F 95 C1 3B C3 88 4C 24 30");
            if (borderlessSwitch.empty())
                return;

            // Where the mode flag is born: one instruction after the SETNZ that derives it from the
            // render config's Fullscreen property and one before the store everything downstream
            // reads. EAX holds the render config, ECX's low byte the flag.
            auto modeFlag = dunia_pattern("39 58 28 89 5C 24 78 0F 95 C1 39 58 2C 88 4C 24 30 0F 95 C0 3A CB");
            if (modeFlag.empty())
                return;

            // The window is created at 640x480 and sized once during engine init by this helper,
            // SetWindowMode(hWnd, title, x, y, width, height, hideCursor, maximized). Overriding
            // its arguments covers the display, and it is the only place the window handle is
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

                *(int32_t*)(regs.esp + 0x0C) = 0;                             // x
                *(int32_t*)(regs.esp + 0x10) = 0;                             // y
                *(int32_t*)(regs.esp + 0x14) = GetSystemMetrics(SM_CXSCREEN); // width
                *(int32_t*)(regs.esp + 0x18) = GetSystemMetrics(SM_CYSCREEN); // height

                // The cursor argument would otherwise follow the mode flag, which borderless has
                // just cleared to get the windowed resolution path. Set back by hand so borderless
                // still feels like fullscreen, rather than leaving the flag on and taking the
                // mode-enumeration path with it.
                *(int32_t*)(regs.esp + 0x1C) = 1;
            });

            static auto DeviceConfigHook = safetyhook::create_mid(deviceConfig.get_first(), [](SafetyHookContext& regs)
            {
                // The device config arrives by pointer in SetResolution's fourth stack argument;
                // the relocated MOV loads it into EBP a moment from now.
                auto* pConfig = *reinterpret_cast<uint8_t**>(regs.esp + 0x264);
                if (!pConfig)
                    return;

                pConfig[8] = IsFullscreen() ? 1 : 0;

                // Maximized is the engine's desktop-sized-window mode. Its branch at 0x104234D1
                // overwrites the requested resolution with the largest mode EnumDisplaySettings
                // reports and sets renderer+0x90, which swaps Present's NULL rects for a
                // client-rect pair: an unscaled top-left blit as soon as the backbuffer is smaller
                // than the window. Borderless uses the plain windowed branch below it.
                pConfig[9] = 0;
            });

            // Renderer vtable+0x134, matched from its prologue through the null-device test that
            // reaches the first of its three failure paths.
            auto isDeviceFullscreen = dunia_pattern("83 EC 3C 83 79 38 00 74 45 8B 41 38 8D 14 24 52");
            if (!isDeviceFullscreen.empty())
                IsDeviceFullscreenHook = safetyhook::create_inline(isDeviceFullscreen.get_first(), IsDeviceFullscreen);

            // Borderless collapsed into a box at the top left: D3D9 sizes the window to
            // BackBufferWidth/Height after every successful Reset, but on WS_POPUP
            // AdjustWindowRectEx is a no-op and SWP_NOMOVE holds the origin, so the window shrinks
            // to the resolution at 0,0.
            //
            // FUN_10422630, at that SetWindowPos, with ECX and EDX holding cy and cx. Anchored on
            // the PUSH of uFlags because the call itself is two bytes shared with the SetWindowPos
            // before it.
            auto resetWindowSize = dunia_pattern("68 06 02 00 00 51 52 55 55 55 50 FF D3");
            if (!resetWindowSize.empty())
            {
                static auto ResetWindowSizeHook = safetyhook::create_mid(resetWindowSize.get_first(), [](SafetyHookContext& regs)
                {
                    // The device has just been reset. If the mode moved since the window was made,
                    // restyle it here, before the engine's own resize, so that one lands on a
                    // window already wearing the right frame.
                    if (nWindowMode != nDisplayMode)
                        ApplyWindowMode(hGameWindow);

                    if (!IsBorderless())
                        return;

                    regs.edx = static_cast<uintptr_t>(GetSystemMetrics(SM_CXSCREEN));
                    regs.ecx = static_cast<uintptr_t>(GetSystemMetrics(SM_CYSCREEN));
                });
            }

            // Shared between the two renderers

            // FUN_1033C7E0, at the store that writes the device config's fullscreen byte from
            // whatever the renderer says the device is doing (videoMgr+0x360, modeParams+0x08). It
            // is read again at 0x1033C9D8 to pick the bordered-window branch, which ends in
            // FUN_10406E20, a search that only ever returns a mode strictly smaller than the one it
            // was given. The relocated MOV reads AL, and the toggle path a few instructions above
            // reaches the same store, so one write here also makes Alt+Enter land on the mode the
            // ini asked for.
            auto fullscreenLatch = dunia_pattern("88 86 60 03 00 00 8B 0D ? ? ? ? 8B 11 8B 82 34 01 00 00");
            if (!fullscreenLatch.empty())
            {
                static auto FullscreenLatchHook = safetyhook::create_mid(fullscreenLatch.get_first(), [](SafetyHookContext& regs)
                {
                    regs.eax = (regs.eax & ~0xFFu) | (IsFullscreen() ? 1u : 0u);
                });
            }

            // FUN_1034CA80, one instruction past the flag that says the window and the configured
            // resolution disagree. Borderless makes them disagree permanently, and all the engine
            // does with it is ask for a device change it does not need. Anchored after the store
            // because every earlier site is a conditional branch and this one is five plain bytes.
            auto pumpDemand = dunia_pattern("B3 01 C6 44 24 34 00 8B 44 24 14 83 F8 01");
            if (!pumpDemand.empty())
            {
                static auto PumpDemandHook = safetyhook::create_mid(pumpDemand.get_first(0x02), [](SafetyHookContext& regs)
                {
                    if (IsBorderless())
                        regs.ebx &= ~0xFFu;
                });
            }

            // Direct3D 10

            // The same collapse on this side: D3D10 never touches the window itself but calls
            // IDXGISwapChain::ResizeTarget at 0x104205EB, which resizes the output window to the
            // mode it is handed.
            //
            // SetMode, at the two loads that fetch ResizeTarget out of the swapchain's vtable. EAX
            // is the swapchain and is reloaded from renderer+0x40 at 0x104205F3, so a substitute
            // put there reaches this one call and nothing after it. EDI is loaded from ESI by the
            // LEA that follows, so ResizeBuffers still reads the real description.
            auto resizeTarget = dunia_pattern("8B 08 8B 51 38 8D 7E 4C 57 50 FF D2");
            if (!resizeTarget.empty())
            {
                pStubSwapChainVtbl[nResizeTarget / 4] = reinterpret_cast<void*>(&NullResizeTarget);

                static auto ResizeTargetHook = safetyhook::create_mid(resizeTarget.get_first(), [](SafetyHookContext& regs)
                {
                    if (!IsBorderless())
                        return;

                    regs.eax = reinterpret_cast<uintptr_t>(pStubSwapChain);
                });
            }

            // SetMode again, at the call that rebuilds the render target views after a resize. Only
            // the displacement is wanted, for the re-entry below to run the same step.
            auto rebuildViews = dunia_pattern("8B CE E8 ? ? ? ? 80 7C 24 13 00 75 0B");
            if (!rebuildViews.empty())
            {
                auto* pCall = rebuildViews.get_first<uint8_t>(0x02);
                pRebuildViews = reinterpret_cast<RebuildViews_t>(pCall + 5 + *reinterpret_cast<int32_t*>(pCall + 1));
            }

            // The tail of the D3D10 end-of-frame function, on the path Present takes when it
            // returns S_OK, after the deferred destroy queues have drained. The eight other callers
            // of the fullscreen poll are queries made mid-decision, or run between a swapchain
            // being created and its views existing; this one has nothing half built.
            auto frameTail = dunia_pattern("8B 86 5C 01 00 00 A3 ? ? ? ? 8B 8E 74 01 00 00");
            if (!frameTail.empty())
            {
                static auto FrameTailHook = safetyhook::create_mid(frameTail.get_first(), [](SafetyHookContext& regs)
                {
                    auto* pRenderer = reinterpret_cast<uint8_t*>(regs.esi);

                    if (IsFullscreen() && *(pRenderer + nRendererFullscreen) == 0)
                        ReenterFullscreen(pRenderer);
                });
            }

            // FUN_1033C7E0 asks for a device reset forever, which was the erratic windowed frame
            // rate. It compares GetClientRect against the config resolution at 0x1033C933 and, on a
            // difference, runs AdjustWindowRectEx over the config resolution and compares that
            // against SM_CXFULLSCREEN / SM_CYFULLSCREEN at 0x1033C9F9 for any non-WS_POPUP window.
            // A bordered window asked for the desktop resolution satisfies neither, and nothing
            // converges: the clamp goes into renderMgr+0x330, which the function zeroes on entry,
            // while the comparison stays on the unclamped config.
            //
            // Hooked after AdjustWindowRectEx and the two GetSystemMetrics calls, with the adjusted
            // extents live in EDI and EBP and the comparison six instructions away, rather than at
            // the comparison, because these six bytes are one whole instruction with no branch.
            auto resizeDemand = dunia_pattern("2B 7C 24 28 2B 6C 24 2C 89 44 24 18 8B 01 8B 90 34 01 00 00 FF D2 84 C0 75 75 81 E3 00 00 00 80");
            if (!resizeDemand.empty())
            {
                static auto ResizeDemandHook = safetyhook::create_mid(resizeDemand.get_first(0x1A), [](SafetyHookContext& regs)
                {
                    // Zero extents make both halves of the comparison succeed, so the function
                    // falls through to its "nothing to do" return. Both registers are dead
                    // afterwards; the epilogue only pops them. Borderless reaches the comparison
                    // already passing, so zeroing changes nothing there.
                    regs.edi = 0;
                    regs.ebp = 0;
                });
            }

            // SetResolution hardcodes PresentationInterval to D3DPRESENT_INTERVAL_IMMEDIATE for any
            // windowed device at 0x104235CF (the config's own vsync only ever reaches the
            // fullscreen branch) and leaves BackBufferCount at zero, which D3D9 reads as one. Only
            // a bordered window takes the composed blit path, where IMMEDIATE buys nothing against
            // a compositor that owns the vblank and one backbuffer absorbs no jitter. Exclusive
            // fullscreen flips, and so does a popup covering the display.
            //
            // Both values live in the present parameters, and the block is only ever addressed
            // absolutely, so its base comes from the operand of the one pair of instructions that
            // write BackBufferWidth and BackBufferHeight.
            auto presentParams = dunia_pattern("8B 56 1C 89 15 ? ? ? ? 8B 46 20 A3 ? ? ? ? 8A 4D 08 F6 D9");
            auto presentTail = dunia_pattern("8B 94 24 7C 02 00 00 8B 84 24 78 02 00 00 8B 8C 24 74 02 00 00");

            if (!presentParams.empty() && !presentTail.empty())
            {
                pPresentParams = reinterpret_cast<D3DPRESENT_PARAMETERS*>(*presentParams.get_first<uint32_t*>(5));

                // The first instruction after SetResolution has finished with the present
                // parameters and before it hands them to the multisample check. This only overrides
                // what the engine had no way to get right, and runs again on every resolution
                // change rather than once.
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
            // maximising a popup that already covers the display snaps it to the work area and lets
            // the taskbar back on top.
            auto maximized = dunia_pattern("8B 51 08 39 5A 2C 74 0A A1 ? ? ? ? 6A 03 50 EB 09");
            if (!maximized.empty())
            {
                static auto MaximizedHook = safetyhook::create_mid(maximized.get_first(), [](SafetyHookContext& regs)
                {
                    if (IsBorderless())
                        regs.ecx = reinterpret_cast<uintptr_t>(pZeroedSettingsManager);
                });
            }

            static constexpr auto ApplyDisplayMode = []()
            {
                nDisplayMode = JackalFixSettings.GetInt(PREF_DISPLAYMODE);
            };

            ApplyDisplayMode();

            // The window is made in this mode, so the reset hook has nothing to do until the
            // setting actually moves.
            nWindowMode = nDisplayMode;

            // The window style, its rect and the boot resolution path are all read during startup,
            // so an ini change lands on the next launch. The device's fullscreen byte and the
            // cursor clip do follow it live.
            JackalFix::onIniFileChange() += ApplyDisplayMode;

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
