module;

#include <common.hxx>
#include <d3d9.h>
#include <intrin.h>

export module borderless;

import common;
import dunia;
import settings;

// D3DPRESENT_PARAMETERS, found through the BackBufferWidth/Height writes; ASLR moves it.
static D3DPRESENT_PARAMETERS* pPresentParams = nullptr;

enum DisplayModeSetting
{
    DISPLAY_FULLSCREEN = 1,
    DISPLAY_BORDERLESS = 2,
    DISPLAY_WINDOWED = 3,
};

// No saved video option is written.
static int32_t nDisplayMode = DISPLAY_BORDERLESS;
static HWND hGameWindow = nullptr;

static bool IsFullscreen() { return nDisplayMode == DISPLAY_FULLSCREEN; }
static bool IsBorderless() { return nDisplayMode == DISPLAY_BORDERLESS; }

// Stands in for the settings manager where Maximized is read raw; zeroes leave the setting alone.
static uint32_t nZeroedConfig[16] = {};
static void* pZeroedSettingsManager[4] = { nullptr, nullptr, nZeroedConfig, nullptr };

// Renderer vtable+0x134. Both renderers answer from a live query and say "windowed" when it fails,
// so alt-tab out of exclusive fullscreen came back windowed. Replaced outright.
static SafetyHookInline IsDeviceFullscreenHook{};

static uint8_t __fastcall IsDeviceFullscreen(void*, void*)
{
    return IsFullscreen() ? 1 : 0;
}

// D3D10 renderer fields off DAT_11609668; its DXGI_SWAP_CHAIN_DESC is embedded at +0x4C.
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

// Re-acquires the backbuffer, rebuilds the RTV, resets the viewport. From SetMode's call.
static RebuildViews_t pRebuildViews = nullptr;

// Stand-in swapchain for the one call borderless does not want made; only vtable+0x38 is read
// through it, and the real pointer is reloaded four instructions later.
static HRESULT __stdcall NullResizeTarget(void*, const void*) { return S_OK; }
static void* pStubSwapChainVtbl[16] = {};
static void* pStubSwapChain[1] = { pStubSwapChainVtbl };

// Style is decided once at creation and the engine's post-reset SetWindowPos carries no
// SWP_FRAMECHANGED, so a mode switch restyles on that reset. Both styles are the engine's own.
static constexpr LONG nWindowedStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
static constexpr LONG nBorderlessStyle = WS_POPUP;
static constexpr LONG nModeStyles = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
                                  | WS_THICKFRAME | WS_POPUP;

// The mode the window is currently wearing, so a reset that changes nothing leaves it alone.
static int32_t nWindowMode = 0;

// The window the game draws into. Null until engine init has sized it.
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
        // Client area must be the backbuffer; centred, since it may arrive from a borderless 0,0.
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

// Nothing calls SetMode after an alt-tab: every check agrees with the windowed state DXGI left.
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

    // The two steps SetMode runs after its own transition; without them the render target view
    // still refers to the pre-change buffers.
    reinterpret_cast<ResizeBuffers_t>(ppVtbl[nResizeBuffers / 4])(pSwapChain,
        *reinterpret_cast<UINT*>(pRenderer + nDescBufferCount),
        *reinterpret_cast<UINT*>(pRenderer + nDescWidth),
        *reinterpret_cast<UINT*>(pRenderer + nDescHeight),
        *reinterpret_cast<UINT*>(pRenderer + nDescFormat),
        *reinterpret_cast<UINT*>(pRenderer + nDescFlags));

    if (pRebuildViews)
        pRebuildViews(pRenderer);
}

// The game imports no ClipCursor, so a borderless window lets the pointer onto a second monitor.
// Own thread: Dunia exposes no window procedure and no SetWindowLongA import to subclass through.
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


// A display setting without a restart: set the render manager's dirty byte and force the "does the
// mode differ" check yes once. Called rather than stubbed, since it also writes the size override;
// spent only on the caller that ends in SetMode, recognised by the address it returns to.

// The render manager's "the profile moved" byte, read by the device tick before anything else.
static constexpr ptrdiff_t nRenderManagerDirty = 0x30;

// The manager's only construction.
static const char* const szRenderManagerPattern =
    "6A 00 68 B8 03 00 00 E8 ? ? ? ? 83 C4 08 85 C0 74 0D 8B C8 E8 ? ? ? ? A3 ? ? ? ?";
static constexpr ptrdiff_t nRenderManagerPointer = 27; // disp32 of mov [<render manager>],eax

// Head of Dunia+354B30: loads the profile, calls the comparison, tests the dirty byte.
static const char* const szModeCheckPattern = "53 56 57 8B 3D ? ? ? ? 57 8B F1 E8 ? ? ? ? 80 7E 30 00";
static constexpr ptrdiff_t nModeCheckCall = 12;       // call <does the mode differ>

static void** ppRenderManager = nullptr;

// After the call at 10354B3C, the only return address a forced yes may be spent on.
static const void* pModeCheckReturn = nullptr;

static SafetyHookInline ModeDiffersHook{};

// Raised when a setting below moves, lowered once the mode change went through. Cross-thread.
static std::atomic<bool> bModeChangePending = false;

// What was in force when the device was last built, so only real movement asks for a broadcast.
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

// Asks the engine to rebuild its device on its next tick; writing the byte is all this does.
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

// Order against the mode hooks and internalres does not matter: the rebuild is only asked for.
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

// The engine's frame rate readout, behind the showFps console variable; nothing else writes it, so
// no hook. The console help string "0 (on) or 1 (off)" is backwards: non-zero draws.
static int32_t* pShowFps = nullptr;

// Into the pattern below, to the CMP's relocated address operand.
static constexpr uintptr_t nShowFpsOperand = 6;

static void InstallFpsCounter()
{
    // The CMP carries nothing but the relocated address, so the anchor reaches back to the FSTP at
    // 104cfd52 and forward through the two globals that follow.
    auto pattern = dunia_pattern("D9 5C 24 0C 83 3D ? ? ? ? 00 74 ? 83 3D ? ? ? ? 00 8B 35 ? ? ? ? 75 07 33 C9");
    if (pattern.empty())
        return;

    pShowFps = *reinterpret_cast<int32_t**>(pattern.get_first(nShowFpsOperand));

    static constexpr auto ApplyFpsCounter = []()
    {
        *pShowFps = JackalFixSettings.GetInt(PREF_FPSCOUNTER) != 0;
    };

    // Written before the console registers the variable, so the setting is what it reports.
    ApplyFpsCounter();

    JackalFix::onIniFileChange() += ApplyFpsCounter;
}

class Borderless
{
public:
    Borderless()
    {
        // Ahead of the handler below, which gives up on patterns this shares none of.
        JackalFix::onDuniaInitEvent() += []()
        {
            InstallModeReapply();
        };

        // Separate for the same reason.
        JackalFix::onDuniaInitEvent() += []()
        {
            InstallFpsCounter();
        };

        JackalFix::onDuniaInitEvent() += []()
        {
            // The strstr result for "-borderless" on its way to the window creator's ninth argument,
            // which picks the style. Overwriting the result, not the SETNZ, keeps both directions.

            auto borderlessSwitch = dunia_pattern("85 C0 8B 84 24 50 04 00 00 0F 95 C1 3B C3 88 4C 24 30");
            if (borderlessSwitch.empty())
                return;

            // Where the mode flag is born, one instruction before the store everything downstream
            // reads. EAX holds the render config, ECX's low byte the flag.
            auto modeFlag = dunia_pattern("39 58 28 89 5C 24 78 0F 95 C1 39 58 2C 88 4C 24 30 0F 95 C0 3A CB");
            if (modeFlag.empty())
                return;

            // SetWindowMode(hWnd, title, x, y, width, height, hideCursor, maximized): sizes the
            // 640x480 window once during init, and the only place the HWND is free.
            auto setWindowMode = dunia_pattern("83 EC 10 8B 44 24 18 56 8B 74 24 18 57 50 56 FF 15 ? ? ? ?");
            if (setWindowMode.empty())
                return;

            // SetResolution loading its device config, one instruction before the three-branch.
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

                // The cursor argument would follow the mode flag borderless just cleared for the
                // windowed resolution path; set back so borderless still feels like fullscreen.
                *(int32_t*)(regs.esp + 0x1C) = 1;
            });

            static auto DeviceConfigHook = safetyhook::create_mid(deviceConfig.get_first(), [](SafetyHookContext& regs)
            {
                // Device config by pointer in SetResolution's fourth stack argument; the relocated
                // MOV loads it into EBP.
                auto* pConfig = *reinterpret_cast<uint8_t**>(regs.esp + 0x264);
                if (!pConfig)
                    return;

                pConfig[8] = IsFullscreen() ? 1 : 0;

                // Maximized overwrites the requested resolution with the largest mode and takes an
                // unscaled top-left blit path; borderless wants the plain windowed branch.
                pConfig[9] = 0;
            });

            // Renderer vtable+0x134, prologue through the null-device test.
            auto isDeviceFullscreen = dunia_pattern("83 EC 3C 83 79 38 00 74 45 8B 41 38 8D 14 24 52");
            if (!isDeviceFullscreen.empty())
                IsDeviceFullscreenHook = safetyhook::create_inline(isDeviceFullscreen.get_first(), IsDeviceFullscreen);

            // D3D9 resizes the window to BackBufferWidth/Height after every Reset, which on
            // WS_POPUP collapsed borderless to a box at the top left. At that SetWindowPos, ECX/EDX.
            auto resetWindowSize = dunia_pattern("68 06 02 00 00 51 52 55 55 55 50 FF D3");
            if (!resetWindowSize.empty())
            {
                static auto ResetWindowSizeHook = safetyhook::create_mid(resetWindowSize.get_first(), [](SafetyHookContext& regs)
                {
                    // Restyle before the engine's own resize, so that one lands on a window already
                    // wearing the right frame.
                    if (nWindowMode != nDisplayMode)
                        ApplyWindowMode(hGameWindow);

                    if (!IsBorderless())
                        return;

                    regs.edx = static_cast<uintptr_t>(GetSystemMetrics(SM_CXSCREEN));
                    regs.ecx = static_cast<uintptr_t>(GetSystemMetrics(SM_CYSCREEN));
                });
            }

            // Shared between the two renderers

            // The store writing the device config's fullscreen byte from the renderer's answer;
            // relocated MOV reads AL. Alt+Enter reaches the same store.
            auto fullscreenLatch = dunia_pattern("88 86 60 03 00 00 8B 0D ? ? ? ? 8B 11 8B 82 34 01 00 00");
            if (!fullscreenLatch.empty())
            {
                static auto FullscreenLatchHook = safetyhook::create_mid(fullscreenLatch.get_first(), [](SafetyHookContext& regs)
                {
                    regs.eax = (regs.eax & ~0xFFu) | (IsFullscreen() ? 1u : 0u);
                });
            }

            // One instruction past the "window and configured resolution disagree" flag, which
            // borderless makes permanent.
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

            // The same collapse here: SetMode calls IDXGISwapChain::ResizeTarget, which resizes the
            // output window. EAX is reloaded from renderer+0x40, so a stub reaches this call only.
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

            // SetMode again, at the call rebuilding the render target views; only the displacement.
            auto rebuildViews = dunia_pattern("8B CE E8 ? ? ? ? 80 7C 24 13 00 75 0B");
            if (!rebuildViews.empty())
            {
                auto* pCall = rebuildViews.get_first<uint8_t>(0x02);
                pRebuildViews = reinterpret_cast<RebuildViews_t>(pCall + 5 + *reinterpret_cast<int32_t*>(pCall + 1));
            }

            // Tail of the D3D10 end-of-frame function, S_OK path after the destroy queues drain.
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

            // Endless reset demand on a bordered window at the desktop resolution (the erratic
            // windowed frame rate): the clamp lands in a field zeroed on entry, the comparison stays
            // unclamped. Hooked after AdjustWindowRectEx, adjusted extents live in EDI and EBP.
            auto resizeDemand = dunia_pattern("2B 7C 24 28 2B 6C 24 2C 89 44 24 18 8B 01 8B 90 34 01 00 00 FF D2 84 C0 75 75 81 E3 00 00 00 80");
            if (!resizeDemand.empty())
            {
                static auto ResizeDemandHook = safetyhook::create_mid(resizeDemand.get_first(0x1A), [](SafetyHookContext& regs)
                {
                    // Zero extents pass both halves of the comparison, so the function returns
                    // "nothing to do"; both registers are dead afterwards.
                    regs.edi = 0;
                    regs.ebp = 0;
                });
            }

            // SetResolution hardcodes IMMEDIATE and BackBufferCount 0 for any windowed device; only
            // a bordered window takes the composed blit path, where that buys nothing.
            auto presentParams = dunia_pattern("8B 56 1C 89 15 ? ? ? ? 8B 46 20 A3 ? ? ? ? 8A 4D 08 F6 D9");
            auto presentTail = dunia_pattern("8B 94 24 7C 02 00 00 8B 84 24 78 02 00 00 8B 8C 24 74 02 00 00");

            if (!presentParams.empty() && !presentTail.empty())
            {
                pPresentParams = reinterpret_cast<D3DPRESENT_PARAMETERS*>(*presentParams.get_first<uint32_t*>(5));

                // After SetResolution has finished with the present parameters, before the
                // multisample check; runs again on every resolution change.
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

            // The last thing InitDuniaEngine does, and the only raw Maximized read. Maximising a
            // popup that already covers the display snaps it to the work area.
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

            // The window is made in this mode, so the reset hook idles until the setting moves.
            nWindowMode = nDisplayMode;

            // Style, rect and boot resolution path are read at startup, so an ini change lands next
            // launch; the fullscreen byte and the cursor clip follow live.
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
