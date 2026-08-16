module;

#include <common.hxx>
#include <d3d9.h>
#include <intrin.h>

export module borderless;

import common;
import dunia;
import settings;

// Fullscreen, borderless and windowed, chosen by DisplayMode and driven entirely through the
// engine's own code paths rather than around them.
//
// Dunia already has all three. It just never exposes the choice coherently: the window style comes
// from an undocumented "-borderless" command-line switch with no entry in the video options, and
// everything else, from which resolution path runs to whether the D3D9 device goes exclusive and
// whether the cursor is hidden, hangs off a single derived flag that the video options own.
//
// That flag is the whole mechanism. InitDuniaEngine computes it once at 0x10005367 from the render
// config's Fullscreen property, registered under that name in FUN_10402210, which writes offset
// 0x28 for "Fullscreen" and 0x2C for "Maximized". From there it fans out to:
//
//   the window position   the saved WindowPos X/Y are only read when neither Fullscreen nor
//                         Maximized is set, so a fullscreen profile leaves the window at 0,0
//   the cursor            SetWindowMode's seventh argument; non-zero means SetCursor(NULL)
//   the resolution path   `0x10F92040`, which picks between GetClientRect and walking
//                         EnumDisplaySettings for the nearest mode <= the request
//   the device            the same byte is field +0x08 of the engine-init params at 0x10F92038,
//                         whose first 0x2C bytes become the renderer's device config, and
//                         D3DPRESENT_PARAMETERS.Windowed, FullScreen_RefreshRateInHz and
//                         PresentationInterval are all derived from it
//
// So one write, early enough, moves all four together. That is what this module does: it overwrites
// the flag at the point it is produced, and lets the engine do the rest. Nothing here reimplements
// a mode.
//
// Three things need saying about the details.
//
// The flag is forced twice, in two different places, and both are necessary. The one at engine
// init drives the window and the boot-time resolution choice. The device config is a copy of
// those params taken at device creation, and the render manager recomputes its fullscreen byte
// from the live device state whenever the video options change, renderer vtable +0x134 being a
// GetPresentParameters call returning Windowed == 0, so without a second write inside
// SetResolution the mode would drift the first time the player touched the video options. The
// second write lands before SetResolution's three-branch block reads the byte, which also keeps the
// aspect ratio coherent: the fullscreen branch takes the config's stored aspect and the windowed
// branches derive it from the resolution, so clearing the byte after the branch, as the old
// borderless option did, produced a windowed device carrying a fullscreen aspect.
//
// Borderless has to defeat Maximized as well. It is a real registered property rather than a dead
// field, and the last thing InitDuniaEngine does is ShowWindow(SW_MAXIMIZE) when it is set. On a
// WS_POPUP window sized to cover the display, maximising snaps it to the work area and the taskbar
// reappears over the game.
//
// None of this writes the user's saved video options. Every write is to a derived flag or to
// the renderer's per-run copy, so turning DisplayMode off restores stock behaviour with nothing
// left behind. The consequence is that the Fullscreen entry in the video options still shows what
// the player saved and no longer decides anything, DisplayMode outranking it.
//
// Two more things, both of which are why nobody ever used windowed mode.
//
// The engine asks for a device reset every time it is asked whether one is needed, forever.
// FUN_1033C7E0 decides that. It calls GetClientRect and compares the result against the render
// config's resolution at 0x1033C933; if they differ it runs AdjustWindowRectEx over the config
// resolution rather than the client one and, for a window that is not WS_POPUP, compares the
// result against SM_CXFULLSCREEN / SM_CYFULLSCREEN at 0x1033C9F9. Bigger than either and it
// returns "device change needed".
//
// Ask for a resolution equal to the desktop, which is the obvious thing to do, and a bordered
// window can never satisfy that, because the frame alone puts it over. The client rect can then
// never equal the config resolution either, so the test is reached on every call and answers the
// same way every time. Nothing converges: the clamp it computes goes into renderMgr+0x330, which
// the function itself zeroes on entry, and the comparison is always against the unclamped config.
// The result is a release/Reset/restore cycle several times a second, forever, which is what the
// low erratic frame rate actually was.
//
// Fullscreen returns before reaching it while the renderer answers the fullscreen query truthfully.
// Borderless takes the WS_POPUP branch, where the adjusted size equals the requested size because a
// popup has no frame, so it passes. A bordered window is the only shape that trips it, and
// exclusive fullscreen wears one too, so the two adjusted extents are zeroed just before the
// comparison in every mode. That is the smallest edit that makes the answer "no change needed"
// without touching the clamp, the config, or the other reasons the function can ask for a reset.
//
// And the presentation parameters are wrong for a composed window. SetResolution hardcodes
// PresentationInterval to D3DPRESENT_INTERVAL_IMMEDIATE for any windowed device at 0x104235CF,
// unconditionally, the config's own vsync value only ever reaching the fullscreen branch, and
// leaves BackBufferCount at zero, which D3D9 reads as one.
//
// Exclusive fullscreen does not care: it flips. Borderless does not care either, because a popup
// covering the whole display gets an independent flip out of the compositor and never touches the
// redirection surface. A bordered window is again the one case that takes the composed blit path,
// and there those two values are the worst possible pair. IMMEDIATE buys nothing, since a composed
// window cannot tear and the compositor owns the vblank, so all it does is let the game run ahead
// and then block inside Present. With a single backbuffer there is nothing to absorb the jitter. So
// windowed asks for INTERVAL_ONE and two backbuffers instead; the ceiling was the refresh rate
// either way.

// Dunia ships two renderers behind one interface. FUN_1033C560 picks between them and stores either
// in the same global, DAT_11609668: a Direct3D 10 renderer with vtable 0x10E52CA8 and a Direct3D 9
// one at 0x10E53158. The video manager above them is shared, so FUN_1033C7E0 and FUN_1034CA80 call
// through the vtable and work with either. Below it nothing is shared. The D3D9 SetResolution,
// device Reset and present parameters have no counterpart in the D3D10 renderer, whose Reset slot
// +0x54 is a stub returning 1 and which drives everything through DXGI.
//
// Alt-tab out of exclusive fullscreen returned windowed, one resolution smaller. Both renderers
// answer "is the device fullscreen" (vtable +0x134) from a live query, and both answer "windowed"
// when they cannot answer at all: D3D9's FUN_10422E50 walks GetSwapChain then GetPresentParameters
// and falls to XOR AL,AL at 0x10422E9E on any failure, D3D10's FUN_1041F710 clears its result on a
// null swapchain or a bad HRESULT from IDXGISwapChain::GetFullscreenState. An alt-tab is when the
// answer is unavailable, and under DXGI it is also true: MakeWindowAssociation is called with flags
// 4, so DXGI keeps its own window hook and takes the swapchain out of fullscreen on focus loss.
//
// FUN_1033C7E0 stores that answer into the device config's fullscreen byte at 0x1033C840, which is
// videoMgr+0x360 and modeParams+0x08, the byte both renderers read to decide whether to build a
// fullscreen device. It reads the same answer again at 0x1033C9D8 to decide whether to run the
// bordered-window branch, which puts the config resolution through AdjustWindowRectEx, finds the
// result larger than SM_CXFULLSCREEN and calls FUN_10406E20, a search that only ever returns a mode
// strictly smaller than the one it was given.
//
// Three writes follow. The config byte is written from DisplayMode rather than from the device, and
// the bordered-window clamp is defeated the way the windowed one already was, by zeroing the
// adjusted extents before the comparison. The third is a re-entry. Neither renderer asks to go back
// to fullscreen on its own, because D3D10's only SetFullscreenState(TRUE) passes the state it
// sampled a moment earlier, so the D3D10 path re-enters at the end of a frame where no D3D object
// is half built.
//
// Borderless collapsed into a box in the top left below the desktop resolution. D3D9 sizes the
// window to BackBufferWidth/Height after every successful Reset, which suits a bordered window
// whose client area is meant to be the resolution. On WS_POPUP AdjustWindowRectEx is a no-op and
// SWP_NOMOVE holds the origin, so the window itself shrinks to the resolution at 0,0. D3D10 never
// touches the window (the only SetWindowPos is in the D3D9 reset, the only MoveWindow in engine
// init) and calls IDXGISwapChain::ResizeTarget at 0x104205EB, which resizes the output window to
// the mode it is handed. Either way it is the window that is small rather than the frame inside
// it.
//
// Borderless keeps the window it was given. D3D9 gets the display size in place of the backbuffer
// size, D3D10 gets a stand-in swapchain for that one call. FUN_1034CA80 then disagrees with the
// configured resolution on every pass and asks for a device change it does not need, so its answer
// is cleared where it is produced. Scaling the frame back up once it and the window are different
// sizes belongs to internalres, which owns the split for both renderers.
//
// The crash on changing resolution from the main menu is a GUI lifetime fault rather than a display
// one and lives in guiduplicates.

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

// Renderer vtable+0x134, FUN_10422E50. Replaced outright rather than corrected on its failure path.
// There is no device state in which the live query is the better answer once DisplayMode owns the
// mode, and the failure path is one XOR and a return with nowhere to compute one.
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

// FUN_1041FE80, which re-acquires the backbuffer, rebuilds the render target view and resets the
// viewport. Taken from the displacement of the call SetMode makes to it, because it has no callers
// outside the renderer and no distinctive prologue.
static RebuildViews_t pRebuildViews = nullptr;

// A stand-in swapchain for the one call borderless does not want made. Only vtable+0x38 is ever
// read through it: the substitution lasts exactly as long as ResizeTarget's argument list, and the
// real pointer is reloaded from renderer+0x40 four instructions later.
static HRESULT __stdcall NullResizeTarget(void*, const void*) { return S_OK; }
static void* pStubSwapChainVtbl[16] = {};
static void* pStubSwapChain[1] = { pStubSwapChainVtbl };

// Switching mode while the game runs.
//
// Everything above decides the mode while the device is being built, and one of those decisions is
// made once and never revisited: the window's style. The window is created during engine init and
// the ninth argument to its creator picks between WS_POPUP and a captioned window, so a window born
// windowed keeps its title bar for the rest of the run no matter what the device does afterwards.
// That is why changing the setting and taking a device reset left borderless looking like a plain
// window: the frame was still the one the game started with.
//
// The reset does not restyle it either: the engine's own post-reset call is a SetWindowPos with
// SWP_NOMOVE and no SWP_FRAMECHANGED, which resizes and nothing more. So the restyle is done here,
// on that same reset, which is the one moment the device is already being rebuilt around it.
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

// The window the game is drawing into, for the parts of the mod that have to measure it. Null until
// engine init has sized it.
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
        // The whole display, which is what the boot path gives it.
        SetWindowPos(hWnd, nullptr, 0, 0,
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    else
    {
        // A captioned window is sized so its *client* area is the backbuffer, which is what the
        // engine's windowed path wants and what the frame is drawn for. Centred, because it may be
        // arriving from a borderless origin of 0,0 with a caption now sitting above it.
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

    // Taking the display back the moment DXGI hands it over is how a transition loop starts.
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

// The game confined the pointer by owning the display and imports no ClipCursor at all, so a
// borderless window lets it walk onto a second monitor mid-firefight. Clipped while the window
// holds focus and released the moment it doesn't, on its own thread because Dunia never exposes
// its window procedure and there is no SetWindowLongA import to subclass through. Fullscreen
// needs none of this, the display mode change does it; windowed wants none of it, the pointer
// belongs to the desktop.
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


// ------------------------------------------------------------------------------------------------
// Making a display setting take effect without a restart.
//
// DisplayMode, InternalResolution and ScalingFilter are all decided while the device is being
// built, so every hook above runs inside the engine's own mode change. What was missing is anything
// to make the engine perform one once the game is up.
//
// The machinery is the engine's. CFCXOptionDisplayPage's apply raises the render settings broadcast
// at Dunia+3F8AB0, and one observer on it belongs to the render manager and is two instructions,
// MOV byte ptr [ECX+30h],1 / RET. The per-frame device tick at Dunia+34CA80 tests that byte and
// calls Dunia+354B30, which asks FUN_1033C7E0 whether the mode differs and only on a yes invokes
// the functor at manager+10Ch, ending in the renderer's SetMode. The reset lands on the following
// tick, between frames, on the thread the engine owns the device from. Two things follow: set the
// byte, and answer that question with yes once.
//
// FUN_1033C7E0 is called rather than stubbed. It measures the window and writes the size override
// at manager+330h/+334h that the mode change then uses, so only its answer is overridden.
//
// The yes has to reach the right caller. FUN_1033C7E0 has exactly two and a yes means different
// things to them:
//
//     10354B30   the functor at manager+10Ch, which ends in SetMode
//     10358410   Dunia+33E2B0, which rebuilds nothing
//
// Both ask on the same tick, so a yes handed to the second is lost and the mode never changes. That
// is what "the setting does nothing until a restart" was. Handler install order was blamed for it
// twice and is not the cause. The override is spent only on the call at 10354B3C, recognised by the
// address it returns to; every other caller gets the engine's own answer and the request stays
// pending until the right one asks.

// The render manager's "the profile moved" byte: what the broadcast observer sets, and what the
// device tick looks at before it asks anything else.
static constexpr ptrdiff_t nRenderManagerDirty = 0x30;

// push 0 / push 3B8h / call <alloc> / add esp,8 / test eax,eax / jz / mov ecx,eax /
// call <render manager ctor> / mov [<render manager>],eax. The manager's only construction.
static const char* const szRenderManagerPattern =
    "6A 00 68 B8 03 00 00 E8 ? ? ? ? 83 C4 08 85 C0 74 0D 8B C8 E8 ? ? ? ? A3 ? ? ? ?";
static constexpr ptrdiff_t nRenderManagerPointer = 27; // disp32 of mov [<render manager>],eax

// The head of Dunia+354B30: it loads the render profile, hands it to the comparison and then tests
// the dirty byte. Both of the things wanted here are in that one sequence.
static const char* const szModeCheckPattern = "53 56 57 8B 3D ? ? ? ? 57 8B F1 E8 ? ? ? ? 80 7E 30 00";
static constexpr ptrdiff_t nModeCheckCall = 12;       // call <does the mode differ>

static void** ppRenderManager = nullptr;

// The instruction after the call at 10354B3C, which is the only return address a forced yes may be
// spent on.
static const void* pModeCheckReturn = nullptr;

static SafetyHookInline ModeDiffersHook{};

// Raised when one of the settings below moves, lowered once the engine has actually gone through
// with the mode change. Read from the engine's threads, written from the file watcher's.
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

// Nothing here depends on running before or after the mode hooks or internalres. The rebuild is not
// performed now; it is asked for, and the engine gets to it on its next tick, by which time every
// handler on the init event has had its turn.
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
                auto* pConfig = *reinterpret_cast<uint8_t**>(regs.esp + 0x264);
                if (!pConfig)
                    return;

                pConfig[8] = IsFullscreen() ? 1 : 0;

                // Maximized is the engine's own desktop-sized-window mode. Its branch at
                // 0x104234D1 overwrites the requested resolution with the largest mode
                // EnumDisplaySettings reports and sets renderer+0x90, which swaps Present's NULL
                // rects for a client-rect pair: an unscaled top-left blit as soon as the backbuffer
                // is smaller than the window. Borderless uses the plain windowed branch below it.
                pConfig[9] = 0;
            });

            // Renderer vtable+0x134, matched from its prologue through the null-device test that
            // reaches the first of its three failure paths.
            auto isDeviceFullscreen = dunia_pattern("83 EC 3C 83 79 38 00 74 45 8B 41 38 8D 14 24 52");
            if (!isDeviceFullscreen.empty())
                IsDeviceFullscreenHook = safetyhook::create_inline(isDeviceFullscreen.get_first(), IsDeviceFullscreen);

            // FUN_10422630, at the arguments of the SetWindowPos that follows a successful device
            // Reset, with ECX and EDX holding cy and cx. Anchored on the PUSH of uFlags because the
            // call itself is two bytes shared with the SetWindowPos before it. uFlags carries
            // SWP_NOMOVE, so the size is the only argument worth overriding.
            auto resetWindowSize = dunia_pattern("68 06 02 00 00 51 52 55 55 55 50 FF D3");
            if (!resetWindowSize.empty())
            {
                static auto ResetWindowSizeHook = safetyhook::create_mid(resetWindowSize.get_first(), [](SafetyHookContext& regs)
                {
                    // The device has just been reset. If the mode moved since the window was made,
                    // this is where the window is put into it, before the engine's own resize, so
                    // that one lands on a window already wearing the right frame.
                    if (nWindowMode != nDisplayMode)
                        ApplyWindowMode(hGameWindow);

                    if (!IsBorderless())
                        return;

                    regs.edx = static_cast<uintptr_t>(GetSystemMetrics(SM_CXSCREEN));
                    regs.ecx = static_cast<uintptr_t>(GetSystemMetrics(SM_CYSCREEN));
                });
            }

            // ------------------------------------------------------------------------------------
            // Shared between the two renderers

            // FUN_1033C7E0, at the store that writes the device config's fullscreen byte from
            // whatever the renderer says the device is doing at that moment. The relocated MOV
            // reads AL, and the toggle path a few instructions above it reaches the same store, so
            // one write here also makes an Alt+Enter land on the mode the ini asked for.
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

            // ------------------------------------------------------------------------------------
            // Direct3D 10

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
            // of the fullscreen poll are queries the engine makes mid-decision, or run between a
            // swapchain being created and its views existing; this one has nothing half built.
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

            // FUN_1033C7E0, immediately after AdjustWindowRectEx and the two GetSystemMetrics
            // calls, with the adjusted window extents live in EDI and EBP and the comparison that
            // decides "device change needed" six instructions away. Chosen over the comparison
            // itself because these six bytes are one whole instruction with no branch in them.
            auto resizeDemand = dunia_pattern("2B 7C 24 28 2B 6C 24 2C 89 44 24 18 8B 01 8B 90 34 01 00 00 FF D2 84 C0 75 75 81 E3 00 00 00 80");
            if (!resizeDemand.empty())
            {
                static auto ResizeDemandHook = safetyhook::create_mid(resizeDemand.get_first(0x1A), [](SafetyHookContext& regs)
                {
                    // Zero extents make both halves of the comparison succeed, so the function
                    // falls through to its ordinary "nothing to do" return. Both registers are
                    // dead afterwards; the epilogue only pops them. Borderless reaches the
                    // comparison already passing, so zeroing changes nothing there.
                    regs.edi = 0;
                    regs.ebp = 0;
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

            // The window is made in this mode, so it starts out wearing it and the reset hook has
            // nothing to do until the setting actually moves.
            nWindowMode = nDisplayMode;

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
