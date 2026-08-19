module;

#include <common.hxx>
#include <algorithm>
#include <utility>
#include <vector>
#include <cstdint>
#include <d3d9.h>
#include <d3d10.h>
#include <dxgi.h>
#include <cstring>

// An internal render resolution independent of the window, the display mode and the backbuffer,
// for supersampling above what the monitor can show or rendering below it and scaling back up.
//
// Borderless uses the same path with no setting of its own. Its window covers the display whatever
// the video options ask for, so any resolution below the desktop has already split the frame from
// the output, and leaving that to the runtime gives a point-sampled stretch to the whole client
// area. The engine's own resolution becomes the internal one and the backbuffer becomes the client
// rect, so the resolve below supplies the fit and the filter.
//
// Dunia has exactly one number for "how big is the frame" and it is welded to the swapchain.
// FUN_10423400 (SetResolution) takes a width and a height, stores them at renderer+0x1C/+0x20,
// derives the aspect ratio into renderer+0x24, and copies the same pair straight into the global
// D3DPRESENT_PARAMETERS as BackBufferWidth/BackBufferHeight. Everything downstream follows the
// stored pair: the viewport is a fraction of it, and every scene render target is allocated from
// the viewport. The two quantities are written four instructions apart from the same registers,
// which is why they cannot diverge on their own and why one choke point is enough to split them.
//
// The split is:
//
//   renderer+0x1C / +0x20   the internal resolution. The engine sizes its viewport, its scene
//                           targets, its shared depth buffer and its final PostFxBlit from this,
//                           so the entire render chain moves together and not one viewport
//                           anywhere needs rescaling.
//   BackBufferWidth/Height  the real output resolution: the window's client area when windowed or
//                           borderless, the adapter mode when fullscreen.
//
// That leaves the backbuffer the wrong size for what the engine draws into it, so the backbuffer
// is substituted. The engine never touches an IDirect3DSurface9* for the backbuffer directly. It
// goes through a 0x20-byte wrapper whose only setter is FUN_1040F190, three instructions long, and
// which caches no dimensions at all: vtable+0x10 is a live GetSize that calls the surface's
// GetDesc on every call. Install a different surface and every size query answers consistently,
// with no shadow fields to keep in sync. So the setter hands the engine an internal-res render
// target instead, and the frame function resolves that target onto the real backbuffer just before
// the engine's screenshot block and Present.
//
// Not letting D3D9 stretch a mismatched backbuffer at Present instead is deliberate. It does
// scale, and Microsoft documents that much, but the documentation specifies no filter and in
// practice it is a point sample, every second pixel discarded going down. Supersampling through it
// is worse than not supersampling at all. This resolve picks its own filter and halves through
// scratch targets for ratios above 2:1, so a bilinear tap is always an exact 2x2 box average.
//
// All three display modes work. Exclusive fullscreen needs nothing special: the fullscreen branch
// of SetResolution already leaves renderer+0x90 clear, and the resolution arriving there has
// already been rounded to a real adapter mode by the boot path, so the backbuffer stays a mode the
// driver will accept. That matters, because CreateDevice's failure path at 0x104252D9 is an
// infinite retry loop rather than an error.
//
// The scale is uniform on both axes. An internal resolution shaped differently from the output,
// 4:3 on a 16:9 display say, is fitted and centred with black bars rather than stretched, and the
// engine is told to compose for a screen the shape of the internal frame, so the geometry it draws
// is the geometry it would draw running natively at that resolution.
//
// Two consequences worth knowing. The HUD and menus are drawn by the engine into the same target
// as the scene, so they are resampled with it: sharper than native when supersampling, softer
// when rendering below the output resolution. And the engine's own screenshot key captures after
// the resolve but sizes its capture from renderer+0x1C/+0x20, so its screenshots come out at the
// internal resolution with the game's hardcoded point filter; external capture is unaffected.

export module internalres;

import common;
import dunia;
import settings;

template<typename T>
static void SafeRelease(T*& ptr)
{
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

// ---------------------------------------------------------------------------------------------
// State

enum ScalingFilter
{
    FILTER_POINT = 0,
    FILTER_BILINEAR = 1,
};

static int32_t nRequestedW = 0;         // from the ini; 0 leaves the game entirely alone
static int32_t nRequestedH = 0;
static int32_t nFilter = FILTER_BILINEAR;
static bool bBorderless = false;        // borderless drives the split even with no ini request

static uint32_t nInternalW = 0;         // what the engine believes the screen is
static uint32_t nInternalH = 0;
static uint32_t nOutputW = 0;           // what the swapchain actually presents
static uint32_t nOutputH = 0;

// The last surface the device confirmed as its own backbuffer. Only a fast path, since the
// identity is re-established from the device whenever an unfamiliar surface turns up, so a device
// that is destroyed and recreated rather than reset needs no special handling.
static IDirect3DSurface9* pKnownBackBuffer = nullptr;
static bool bSubstituted = false;

// The two present-parameter globals, reached through the pattern's own operands. ASLR moves them,
// so a hardcoded address would be wrong the moment the module rebases.
static uint32_t* pBackBufferWidth = nullptr;
static uint32_t* pBackBufferHeight = nullptr;

// ---------------------------------------------------------------------------------------------
// The resolve pass

class CResolver
{
    struct ScreenVertex { float x, y, z, rhw, u, v; };
    static constexpr DWORD nScreenFVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

    // Four halvings covers 16x per axis, far past anything a GPU will render Far Cry 2 at.
    static constexpr size_t nMaxChain = 4;

    struct Target
    {
        IDirect3DTexture9* pTex = nullptr;
        IDirect3DSurface9* pSurf = nullptr;
        UINT nWidth = 0;
        UINT nHeight = 0;
        D3DFORMAT eFormat = D3DFMT_UNKNOWN;

        void Release()
        {
            SafeRelease(pSurf);
            SafeRelease(pTex);
            nWidth = 0;
            nHeight = 0;
            eFormat = D3DFMT_UNKNOWN;
        }

        bool Ensure(IDirect3DDevice9* pDevice, UINT w, UINT h, D3DFORMAT fmt)
        {
            if (pTex && pSurf && nWidth == w && nHeight == h && eFormat == fmt)
                return true;

            Release();

            if (FAILED(pDevice->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt,
                                              D3DPOOL_DEFAULT, &pTex, nullptr)))
                return false;

            if (FAILED(pTex->GetSurfaceLevel(0, &pSurf)))
            {
                Release();
                return false;
            }

            nWidth = w;
            nHeight = h;
            eFormat = fmt;
            return true;
        }
    };

    // The surface handed to the engine in place of the backbuffer.
    static inline Target Substitute;
    static inline Target Chain[nMaxChain];

    static inline IDirect3DStateBlock9* pStateBlock = nullptr;
    // One textured quad from pSrc into a rect of pDst. The rect is the whole target for every
    // stage of the halving chain and, for the final pass, the aspect-preserving fit inside the
    // backbuffer, so bars are the only thing that ever separates the two.
    static bool Blit(IDirect3DDevice9* pDevice, IDirect3DSurface9* pDst, UINT nTargetW, UINT nTargetH,
                     int32_t nDstX, int32_t nDstY, UINT nDstW, UINT nDstH,
                     IDirect3DTexture9* pSrc, int32_t eFilter)
    {
        // Depth goes first every time. D3D9 rejects a render target smaller than the depth-stencil
        // currently bound to it, and while supersampling the engine's depth buffer is larger than
        // every target in this chain.
        pDevice->SetDepthStencilSurface(nullptr);

        if (FAILED(pDevice->SetRenderTarget(0, pDst)))
            return false;

        // The viewport is the whole target rather than the fitted rect, so the clear below reaches
        // the bars. The quad is positioned instead.
        const D3DVIEWPORT9 viewport = { 0, 0, nTargetW, nTargetH, 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);

        // Only when there is something outside the image. The swapchain is D3DSWAPEFFECT_DISCARD,
        // so the bars hold whatever the driver last left there unless they are written every frame.
        if (nDstX != 0 || nDstY != 0 || nDstW != nTargetW || nDstH != nTargetH)
            pDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);

        pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
        pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_CLIPPING, TRUE);
        pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);
        pDevice->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
        // The frame arriving here is already in whatever space the game finished in, the engine's
        // own generic surface copy never writing sRGB either, so converting on the way out would
        // shift every pixel.
        pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        for (DWORD i = 1; i < 8; ++i)
            pDevice->SetTexture(i, nullptr);

        const DWORD nSampler = (eFilter == FILTER_POINT) ? D3DTEXF_POINT : D3DTEXF_LINEAR;

        pDevice->SetTexture(0, pSrc);
        pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, nSampler);
        pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, nSampler);
        pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

        pDevice->SetVertexShader(nullptr);
        pDevice->SetIndices(nullptr);
        pDevice->SetFVF(nScreenFVF);

        // A state block restores these afterwards rather than neutralising them first, and between
        // them they account for most of the ways a fullscreen quad comes out wrong. Wrapping makes
        // the 0-to-1 texcoord take the short way round.
        pDevice->SetRenderState(D3DRS_WRAP0, 0);
        pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

        pDevice->SetPixelShader(nullptr);
        pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        const auto fLeft = static_cast<float>(nDstX) - 0.5f;
        const auto fTop = static_cast<float>(nDstY) - 0.5f;
        const auto fRight = fLeft + static_cast<float>(nDstW);
        const auto fBottom = fTop + static_cast<float>(nDstH);

        // D3D9's half-pixel texel-centre offset, folded into the rect's own origin.
        const ScreenVertex verts[4] =
        {
            { fLeft,  fTop,    0.0f, 1.0f, 0.0f, 0.0f },
            { fRight, fTop,    0.0f, 1.0f, 1.0f, 0.0f },
            { fLeft,  fBottom, 0.0f, 1.0f, 0.0f, 1.0f },
            { fRight, fBottom, 0.0f, 1.0f, 1.0f, 1.0f },
        };

        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ScreenVertex));

        // Never leave the source bound: the next stage of the chain renders into it.
        pDevice->SetTexture(0, nullptr);
        return true;
    }

public:
    static IDirect3DSurface9* Surface() { return Substitute.pSurf; }

    static bool Ensure(IDirect3DDevice9* pDevice, UINT w, UINT h, D3DFORMAT fmt)
    {
        return Substitute.Ensure(pDevice, w, h, fmt);
    }

    // Called at the top of the frame function, before the engine's screenshot block and Present.
    // Present is outside the scene bracket, DAT_11609D78 having no write anywhere inside
    // 0x10424150, which is what makes it legal to insert draw work here.
    static void Resolve(IDirect3DDevice9* pDevice)
    {
        if (!pDevice || !Substitute.pSurf)
            return;

        IDirect3DSurface9* pBackBuffer = nullptr;
        if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) || !pBackBuffer)
            return;

        // Sizes matching is not a reason to skip the copy: whatever the engine drew is in the
        // substitute and nothing else is going to put it in front of the player.
        D3DSURFACE_DESC desc{};
        pBackBuffer->GetDesc(&desc);

        // The largest rect of the backbuffer that still has the internal frame's shape, centred.
        // A 4:3 internal resolution on a 16:9 display gets pillarbox bars rather than a stretch, so
        // the scale is uniform on both axes and a circle in the game stays a circle. Integer
        // arithmetic throughout: the rounding decides a pixel of bar width, nothing more. Derived
        // from the substitute rather than from the chain's output, because the halving stages round
        // and the frame's true shape is the one the engine rendered.
        UINT nFitW = desc.Width;
        UINT nFitH = desc.Height;
        {
            const auto w = static_cast<uint64_t>(Substitute.nWidth);
            const auto h = static_cast<uint64_t>(Substitute.nHeight);

            if (w * desc.Height > h * desc.Width)
                nFitH = (std::max)(1u, static_cast<UINT>((desc.Width * h + w / 2) / w));
            else if (h * desc.Width > w * desc.Height)
                nFitW = (std::max)(1u, static_cast<UINT>((desc.Height * w + h / 2) / h));
        }

        const auto nFitX = static_cast<int32_t>((desc.Width - nFitW) / 2);
        const auto nFitY = static_cast<int32_t>((desc.Height - nFitH) / 2);

        // Once the substitute is installed, nothing else in the process puts a frame in front of
        // the player, so no path out of here may leave the backbuffer untouched. StretchRect is
        // the fallback: whatever the driver's filter turns out to be, a scaled frame beats a black
        // one, and it needs no device state at all.
        if (!pStateBlock && FAILED(pDevice->CreateStateBlock(D3DSBT_ALL, &pStateBlock)))
        {
            // ColorFill rather than Clear, because this path is here precisely because no device
            // state can be saved and restored, and ColorFill needs none.
            if (nFitW != desc.Width || nFitH != desc.Height)
                pDevice->ColorFill(pBackBuffer, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));

            const RECT fit = { nFitX, nFitY, nFitX + static_cast<LONG>(nFitW), nFitY + static_cast<LONG>(nFitH) };
            pDevice->StretchRect(Substitute.pSurf, nullptr, pBackBuffer, &fit, D3DTEXF_LINEAR);
            SafeRelease(pBackBuffer);
            return;
        }

        pStateBlock->Capture();

        // A state block does not cover render targets, so those are saved by hand.
        IDirect3DSurface9* pOldTarget = nullptr;
        IDirect3DSurface9* pOldDepth = nullptr;
        pDevice->GetRenderTarget(0, &pOldTarget);
        pDevice->GetDepthStencilSurface(&pOldDepth);

        const bool bOpenedScene = SUCCEEDED(pDevice->BeginScene());

        IDirect3DTexture9* pSrc = Substitute.pTex;
        UINT nSrcW = Substitute.nWidth;
        UINT nSrcH = Substitute.nHeight;

        // A bilinear tap is an exact 2x2 box average only at exactly half size, so anything beyond
        // 2:1 is halved through scratch targets first. Four taps cannot represent a sixteen-texel
        // footprint, and a single stretch across that gap is what makes naive supersampling look
        // no better than none. The target is the fitted rect rather than the backbuffer: with
        // bars, the reduction the final pass has to cover is the one that ends at the rect.
        for (size_t stage = 0; stage < nMaxChain; ++stage)
        {
            // Either axis still being at least 2:1 is reason enough to halve. Requiring both would
            // let a frame that is far oversampled horizontally and not at all vertically skip the
            // chain entirely and take the whole reduction on four taps.
            if (nSrcW < nFitW * 2 && nSrcH < nFitH * 2)
                break;

            const UINT dw = (nSrcW >= nFitW * 2) ? (std::max)(nSrcW / 2, nFitW) : nSrcW;
            const UINT dh = (nSrcH >= nFitH * 2) ? (std::max)(nSrcH / 2, nFitH) : nSrcH;

            if (!Chain[stage].Ensure(pDevice, dw, dh, Substitute.eFormat))
                break;

            // A stage that did not run leaves its target holding the previous frame, so the chain
            // stops here rather than sampling it.
            if (!Blit(pDevice, Chain[stage].pSurf, dw, dh, 0, 0, dw, dh, pSrc, FILTER_BILINEAR))
                break;

            pSrc = Chain[stage].pTex;
            nSrcW = dw;
            nSrcH = dh;
        }

        Blit(pDevice, pBackBuffer, desc.Width, desc.Height, nFitX, nFitY, nFitW, nFitH, pSrc, nFilter);

        if (bOpenedScene)
            pDevice->EndScene();

        // Both or neither. Slot 0 can never be unbound, so a failed GetRenderTarget would leave
        // the output-sized backbuffer bound, and binding the engine's larger depth buffer against
        // it would be rejected, costing the rest of the frame its depth.
        if (pOldTarget)
        {
            pDevice->SetRenderTarget(0, pOldTarget);
            pDevice->SetDepthStencilSurface(pOldDepth);
        }

        pStateBlock->Apply();

        SafeRelease(pOldDepth);
        SafeRelease(pOldTarget);
        SafeRelease(pBackBuffer);
    }

    // Runs at the entry of the engine's pre-reset release pass, the one point where the ordering
    // is guaranteed. Every D3DPOOL_DEFAULT resource has to be gone before Reset, and the device's
    // own render-target binding holds a reference to the substitute: left bound, Reset returns
    // D3DERR_INVALIDCALL, the device never comes back, and the screen stays black for good after
    // the first alt-tab or video-options change.
    //
    // The wrapper's reference is deliberately not dropped and the surface deliberately left
    // installed. The engine dereferences wrapper+0x1C at 0x1042471B with no null check and calls
    // Release on it, and that release is what finally destroys the target.
    static void ReleaseForReset(IDirect3DDevice9* pDevice)
    {
        pKnownBackBuffer = nullptr;

        if (pDevice)
        {
            // Depth first: binding a render target smaller than the currently bound depth buffer
            // is rejected, and while supersampling the engine's depth buffer is the larger of the
            // two.
            pDevice->SetDepthStencilSurface(nullptr);

            IDirect3DSurface9* pBackBuffer = nullptr;
            if (SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) && pBackBuffer)
            {
                pDevice->SetRenderTarget(0, pBackBuffer);
                pBackBuffer->Release();
            }

            for (DWORD i = 1; i < 4; ++i)
                pDevice->SetRenderTarget(i, nullptr);

            pDevice->SetTexture(0, nullptr);
        }

        for (auto& target : Chain)
            target.Release();

        // Only this module's own two references. The wrapper keeps the third, and the engine's
        // unconditional Release four instructions from here is what takes it to zero.
        Substitute.Release();

        SafeRelease(pStateBlock);
    }

    // Process teardown. The device may already be gone, so nothing is asked of it.
    static void ReleaseAll()
    {
        pKnownBackBuffer = nullptr;

        for (auto& target : Chain)
            target.Release();

        Substitute.Release();
        SafeRelease(pStateBlock);
    }
};

// ---------------------------------------------------------------------------------------------
// Hook bodies

// Renderer vtable +0x40, the end-of-frame function. `this` is in ECX at the entry and
// renderer+0x38 is the IDirect3DDevice9*.
static void OnFrameEnd(uintptr_t nRenderer)
{
    if (!bSubstituted || !nRenderer)
        return;

    CResolver::Resolve(*reinterpret_cast<IDirect3DDevice9**>(nRenderer + 0x38));
}

// FUN_1040F190, the wrapper's surface setter. It is generic, every render-target wrapper in the
// engine going through it, so the backbuffer has to be recognised rather than assumed. The test is
// on the surface rather than on the wrapper object, because "this is the swapchain's own surface"
// is exactly the condition that should be substituted, and because it survives a device being
// destroyed and rebuilt rather than reset.
static void OnSetWrapperSurface(IDirect3DSurface9** ppSurface)
{
    if (nInternalW == 0 || nInternalH == 0 || nOutputW == 0 || nOutputH == 0)
        return;

    // Nothing to gain, and substituting would only cost a copy.
    if (nInternalW == nOutputW && nInternalH == nOutputH)
        return;

    IDirect3DSurface9* pIncoming = *ppSurface;

    // The wrapper is torn down and set to NULL twice a frame. Those pass straight through.
    if (!pIncoming)
        return;

    // Every other render-target wrapper in the engine comes through here too, so the common case
    // has to cost a pointer compare rather than two calls into the runtime. The identity is only
    // re-established when there is none, which the pre-reset teardown arranges.
    if (pKnownBackBuffer && pIncoming != pKnownBackBuffer)
        return;

    IDirect3DDevice9* pDevice = nullptr;
    if (FAILED(pIncoming->GetDevice(&pDevice)) || !pDevice)
        return;

    if (pIncoming != pKnownBackBuffer)
    {
        IDirect3DSurface9* pBackBuffer = nullptr;
        const bool bIsBackBuffer =
            SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) &&
            pBackBuffer == pIncoming;

        if (pBackBuffer)
            pBackBuffer->Release();

        if (!bIsBackBuffer)
        {
            pDevice->Release();
            return;
        }

        pKnownBackBuffer = pIncoming;
    }

    D3DSURFACE_DESC desc{};
    pIncoming->GetDesc(&desc);

    if (CResolver::Ensure(pDevice, nInternalW, nInternalH, desc.Format))
    {
        auto* pSubstitute = CResolver::Surface();

        // Standing in for the GetBackBuffer the engine just made. The wrapper owns exactly one
        // reference to whatever is installed and releases it unconditionally next frame, so the
        // reference it was handed has to be replaced rather than added to.
        pSubstitute->AddRef();
        *ppSurface = pSubstitute;
        pIncoming->Release();

        bSubstituted = true;
    }
    else
    {
        // The engine keeps the real backbuffer. Its viewport and its scene targets are still sized
        // for the internal resolution, so the frame will be wrong. Wrong and visible beats a
        // resolve pass firing against a target that no longer exists.
        bSubstituted = false;
    }

    pDevice->Release();
}

class InternalResolution
{
public:
    InternalResolution()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // FUN_10423400. Width and height arrive by pointer and point at the caller's own stack
            // slots, which 0x10423610 reads back after the call to set the viewport, so a write
            // through them at the entry reaches the viewport, the renderer fields, the aspect
            // ratio and the present parameters from a single place.
            auto setResolution = dunia_pattern("81 EC 48 02 00 00 53 55 56 57 33 ED 55 8D 84 24 B4 00 00 00");
            if (setResolution.empty())
                return;

            // The two present-parameter writes, the only writers of those globals in the binary.
            // Hooking past them rather than in place of them leaves the engine's own bookkeeping
            // intact and corrects nothing but the backbuffer size.
            auto presentParams = dunia_pattern("8B 56 1C 89 15 ? ? ? ? 8B 46 20 A3 ? ? ? ? 8A 4D 08 F6 D9");
            if (presentParams.empty())
                return;

            // FUN_1040F190. The pattern runs past the end of its ten-byte body into the CC padding
            // and the next prologue on purpose: an identical setter body exists at 0x102795C4.
            auto wrapperSetter = dunia_pattern("8B 44 24 04 89 41 1C C2 04 00 CC CC CC CC CC CC 56 57 8B F9 33 F6 39 77");
            if (wrapperSetter.empty())
                return;

            // Renderer vtable +0x40, the end-of-frame function.
            auto frameEnd = dunia_pattern("81 EC 80 00 00 00 53 55 56 8B F1 8B 46 78 8B 48 14 8B 41 1C 8B 10 57 50");
            if (frameEnd.empty())
                return;

            // Renderer vtable +0x50, the pre-reset release pass.
            auto preReset = dunia_pattern("A1 ? ? ? ? 83 EC 18 53 55 56 8B 35 ? ? ? ? 8D 04 80 57 8D 3C 86 3B F7 8B D9 C6 05 ? ? ? ? 00 74 10");
            if (preReset.empty())
                return;

            pBackBufferWidth = *presentParams.get_first<uint32_t*>(5);
            pBackBufferHeight = *presentParams.get_first<uint32_t*>(13);

            static auto SetResolutionHook = safetyhook::create_mid(setResolution.get_first(), [](SafetyHookContext& regs)
            {
                // At the first instruction [esp+0] is still the return address.
                auto* pWidth = *reinterpret_cast<uint32_t**>(regs.esp + 0x04);
                auto* pHeight = *reinterpret_cast<uint32_t**>(regs.esp + 0x08);
                auto* pConfig = *reinterpret_cast<uint8_t**>(regs.esp + 0x0C);

                nInternalW = 0;
                nInternalH = 0;

                if (!pWidth || !pHeight || !pConfig)
                    return;

                auto nWantW = nRequestedW;
                auto nWantH = nRequestedH;

                // Borderless keeps a window covering the display whatever resolution the video
                // options ask for, so below the desktop the two quantities have already come apart
                // and something has to put the frame back on the screen. Left to the runtime that
                // is a stretch to the whole client area: the wrong shape at any aspect ratio the
                // display does not share, and a point sample at every ratio. Routing it through
                // here instead costs one blit and gets the fit and the filter that the explicit
                // internal resolution already had.
                if ((nWantW <= 0 || nWantH <= 0) && bBorderless)
                {
                    // Only once the two have actually come apart. At the desktop resolution the
                    // split would be a full screen blit per frame to reproduce the frame exactly,
                    // so borderless at native stays byte for byte stock. Measured rather than
                    // assumed: the window is the mod's own and the video options are the player's,
                    // and neither is a safe guess about the other.
                    RECT client{};
                    auto hWnd = *reinterpret_cast<HWND*>(pConfig + 0x18);
                    if (!hWnd)
                        hWnd = *reinterpret_cast<HWND*>(pConfig + 0x04);

                    if (hWnd && GetClientRect(hWnd, &client) &&
                        (client.right - client.left != static_cast<LONG>(*pWidth) ||
                         client.bottom - client.top != static_cast<LONG>(*pHeight)))
                    {
                        nWantW = static_cast<int32_t>(*pWidth);
                        nWantH = static_cast<int32_t>(*pHeight);
                    }
                }

                const bool bWanted = nWantW > 0 && nWantH > 0;

                // What the engine was about to render at is what it should still present at. In
                // fullscreen that has already been rounded down to a real adapter mode by the boot
                // path, so it stays a mode CreateDevice will accept. That matters, because a mode
                // it will not accept is an infinite retry loop rather than an error. Windowed, it
                // gets replaced with the measured client rect once the branch is known.
                nOutputW = *pWidth;
                nOutputH = *pHeight;

                if (!bWanted || nOutputW == 0 || nOutputH == 0)
                    return;

                // "Use the desktop resolution" makes GetScreenSize hand back the window's client
                // rect instead of the stored pair, which silently undoes the whole split. Clearing
                // it costs nothing: the backbuffer is sized from that same client rect either way.
                pConfig[9] = 0;

                nInternalW = static_cast<uint32_t>(nWantW);
                nInternalH = static_cast<uint32_t>(nWantH);

                *pWidth = nInternalW;
                *pHeight = nInternalH;
            });

            static auto BackBufferSizeHook = safetyhook::create_mid(presentParams.get_first(0x11), [](SafetyHookContext& regs)
            {
                if (nInternalW == 0 || nInternalH == 0 || nOutputW == 0 || nOutputH == 0 ||
                    !pBackBufferWidth || !pBackBufferHeight)
                    return;

                // EBP is the device config and ESI the renderer at this point in the function. The
                // hook sits on the instruction that reads the fullscreen byte for the presentation
                // interval; DAT_11609D40 (Windowed) was derived from the same byte a few
                // instructions back, and the three-branch block read it before that. Nothing writes
                // it in between, so one reading here answers for both.
                auto* pConfig = reinterpret_cast<uint8_t*>(regs.ebp);
                const bool bFullscreen = pConfig[8] != 0;

                if (!bFullscreen)
                {
                    // Measure the window rather than recompute it, so the backbuffer and the
                    // client area cannot drift apart, including in borderless, where the window
                    // covers the display no matter what resolution the video options say.
                    HWND hWnd = *reinterpret_cast<HWND*>(pConfig + 0x18);
                    if (!hWnd)
                        hWnd = *reinterpret_cast<HWND*>(pConfig + 0x04);

                    RECT client{};
                    if (hWnd && GetClientRect(hWnd, &client) &&
                        client.right > client.left && client.bottom > client.top)
                    {
                        nOutputW = static_cast<uint32_t>(client.right - client.left);
                        nOutputH = static_cast<uint32_t>(client.bottom - client.top);
                    }
                }

                *pBackBufferWidth = nOutputW;
                *pBackBufferHeight = nOutputH;

                // The frame is presented letterboxed, so its pixels are square on the display and
                // the engine should compose for a screen shaped like the internal frame, which is
                // exactly what both windowed branches already derived from the resolution. Only the
                // fullscreen branch differs: it takes the config's stored aspect, which describes
                // the monitor rather than the frame.
                //
                // Left alone when the two shapes agree, so a matched internal resolution is
                // byte-for-byte stock and the video options' own forced-aspect setting still means
                // what it says.
                const auto fInternal = static_cast<float>(nInternalW) / static_cast<float>(nInternalH);
                const auto fOutput = static_cast<float>(nOutputW) / static_cast<float>(nOutputH);

                if (bFullscreen && std::fabs(fInternal - fOutput) > 0.0001f)
                    *reinterpret_cast<float*>(regs.esi + 0x24) = fInternal;
            });

            static auto WrapperSetterHook = safetyhook::create_mid(wrapperSetter.get_first(), [](SafetyHookContext& regs)
            {
                // __thiscall f(this in ECX, surface at [esp+4]), callee-cleaned. Rewriting the
                // stack slot is enough: the relocated MOV EAX,[ESP+4] reads it back afterwards.
                OnSetWrapperSurface(reinterpret_cast<IDirect3DSurface9**>(regs.esp + 0x04));
            });

            static auto FrameEndHook = safetyhook::create_mid(frameEnd.get_first(), [](SafetyHookContext& regs)
            {
                OnFrameEnd(regs.ecx);
            });

            static auto PreResetHook = safetyhook::create_mid(preReset.get_first(), [](SafetyHookContext& regs)
            {
                // Unconditional. Nothing this module owns may outlive the Reset four instructions
                // from here, whether or not a substitution is currently installed.
                CResolver::ReleaseForReset(regs.ecx ? *reinterpret_cast<IDirect3DDevice9**>(regs.ecx + 0x38) : nullptr);
                bSubstituted = false;
            });

            static auto InternalResolutionCB = []()
            {
                nRequestedW = JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONX);
                nRequestedH = JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONY);
                nFilter = JackalFixSettings.GetInt(PREF_SCALINGFILTER);
                bBorderless = JackalFixSettings.GetInt(PREF_DISPLAYMODE) == 2;

                // One axis without the other is a typo rather than a request.
                if (nRequestedW <= 0 || nRequestedH <= 0)
                {
                    nRequestedW = 0;
                    nRequestedH = 0;
                }
            };

            InternalResolutionCB();

            // The resolution is read at engine start and again whenever the video options apply a
            // new one, so an ini change lands on the next launch or the next resolution change.
            JackalFix::onIniFileChange() += []()
            {
                InternalResolutionCB();
            };
        };

        JackalFix::onShutdownEvent() += []()
        {
            CResolver::ReleaseAll();
        };
    }
} InternalResolution;

// =================================================================================================
// Direct3D 10
//
// The same split against the other renderer. Nothing above this line runs here: it is written to
// IDirect3DDevice9, the global D3DPRESENT_PARAMETERS, FUN_1040F190's surface wrapper, StretchRect,
// state blocks and ps_3_0, and the D3D10 renderer shares only the vtable above all of it.
//
// Where each quantity lives:
//
//   renderer+0x1C/+0x20        the resolution the video options asked for. FUN_1041FB00 writes it
//                              from SetMode's arguments and nothing else does, so it survives the
//                              swapchain description being rewritten underneath it.
//   vtable+0x44                the size the engine allocates from: its viewport, its scene targets
//                              and its depth buffer. Answered with the internal resolution, except
//                              for EndFrame refilling the swapchain description at 0x10420D6F,
//                              the only caller that passes the description's own fields as the
//                              out-pointers and the only one that wants the real size.
//   the swapchain buffers      resized to the client area at the entry of FUN_1041FE80, the only
//                              place GetBuffer is called and the one point reached after creation
//                              as well as after a resize.
//   the render target          substituted at the call that attaches the backbuffer texture to its
//                              wrapper, so the engine draws into an internal-sized target and the
//                              real backbuffer is left for the resolve.
//
// The substitution is one instruction wide. FUN_10418290 is the counterpart of FUN_1040F190:
// it stores the texture at wrapper+0x2C and builds the views from the texture's own BindFlags, so
// a target created with RENDER_TARGET and SHADER_RESOURCE arrives carrying both an RTV and an SRV.
// Being class-wide it is hooked at its call site inside FUN_1041FE80, and only the argument in ECX
// is replaced. The stack slot the engine releases afterwards still holds the real backbuffer, and
// without that release IDXGISwapChain::ResizeBuffers fails for the rest of the run.
//
// The viewport comes from the swapchain description rather than from the size query, so
// FUN_1041FE80's SetViewport is overridden separately. Its two arguments sit in EBX and EBP eight
// instructions earlier and are dead everywhere else in the function.
//
// The resolve runs at 0x10420DC4, after the frame is drawn and before Present, the only point
// in that function where no D3D object is half built. It takes the backbuffer from GetBuffer(0)
// every frame: a held reference makes the next ResizeBuffers fail with DXGI_ERROR_INVALID_CALL,
// which is the failure FUN_1041FE20 exists to avoid. FUN_10442A90 zeroes the engine's state shadow
// immediately after Present, so everything disturbed here is re-issued next frame. The exception
// is a slot whose next wanted value is also null, which compares equal to the zeroed shadow and is
// skipped, so those two are unbound by hand.
//
// Exclusive fullscreen needs no resize. Its buffers are already the adapter mode, the size that
// reaches the display, so only the substitution and the resolve apply. Windowed and borderless are
// where the buffers have to be pushed out to the client rect first. The decision is taken once per
// swapchain, at FUN_1041FE80, so an ini change lands on the next resolution change.

namespace dx10
{

// Renderer fields, off the object DAT_11609668 points at. The D3D9 renderer is stored in the same
// global behind the same vtable layout, which is why every renderer-agnostic caller works with
// either and why only the implementations below the vtable had to be written twice.
static constexpr uintptr_t nRendererModeWidth = 0x1C;
static constexpr uintptr_t nRendererModeHeight = 0x20;
static constexpr uintptr_t nRendererDevice = 0x38;
static constexpr uintptr_t nRendererSwapChain = 0x40;
static constexpr uintptr_t nDescWidth = 0x4C;
static constexpr uintptr_t nDescOutputWindow = 0x78;
static constexpr uintptr_t nDescWindowed = 0x7C;

static uint32_t nInternalW = 0;
static uint32_t nInternalH = 0;
static uint32_t nOutputW = 0;
static uint32_t nOutputH = 0;
static bool bActive = false;

// ---------------------------------------------------------------------------------------------
// Shaders

// The quad is three vertices generated from SV_VertexID rather than a vertex buffer, so the resolve
// needs no buffer, no input layout and no vertex declaration. IASetInputLayout(nullptr) is legal
// for a vertex shader with no inputs. The viewport is the fitted rect, so the triangle covers
// exactly the area the frame belongs in and the bars are whatever the clear left.
static constexpr const char* szVertexShader = R"(
void ResolveVS(uint id : SV_VertexID, out float4 pos : SV_POSITION, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
)";

// Point and bilinear are the sampler's job, so one shader covers both.
static constexpr const char* szBlitShader = R"(
Texture2D SrcTex : register(t0);
SamplerState SrcSamp : register(s0);

float4 ResolvePS(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(SrcTex.Sample(SrcSamp, uv).rgb, 1.0f);
}
)";

using D3D10CompileShader_t = HRESULT(WINAPI*)(LPCSTR, SIZE_T, LPCSTR, const void*, void*,
                                              LPCSTR, LPCSTR, UINT, ID3D10Blob**, ID3D10Blob**);

// ---------------------------------------------------------------------------------------------
// The resolve pass

class CResolver
{
    static constexpr size_t nMaxChain = 4;

    struct Target
    {
        ID3D10Texture2D* pTex = nullptr;
        ID3D10RenderTargetView* pRtv = nullptr;
        ID3D10ShaderResourceView* pSrv = nullptr;
        UINT nWidth = 0;
        UINT nHeight = 0;
        DXGI_FORMAT eFormat = DXGI_FORMAT_UNKNOWN;

        void Release()
        {
            SafeRelease(pSrv);
            SafeRelease(pRtv);
            SafeRelease(pTex);
            nWidth = 0;
            nHeight = 0;
            eFormat = DXGI_FORMAT_UNKNOWN;
        }

        bool Ensure(ID3D10Device* pDevice, UINT w, UINT h, DXGI_FORMAT fmt)
        {
            if (pTex && pRtv && pSrv && nWidth == w && nHeight == h && eFormat == fmt)
                return true;

            Release();

            D3D10_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = fmt;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D10_USAGE_DEFAULT;
            // Both, because the wrapper builds its views from these and the resolve has to read
            // back what the engine drew.
            desc.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;

            if (FAILED(pDevice->CreateTexture2D(&desc, nullptr, &pTex)) || !pTex)
                return false;

            if (FAILED(pDevice->CreateRenderTargetView(pTex, nullptr, &pRtv)) ||
                FAILED(pDevice->CreateShaderResourceView(pTex, nullptr, &pSrv)))
            {
                Release();
                return false;
            }

            nWidth = w;
            nHeight = h;
            eFormat = fmt;
            return true;
        }
    };

    static inline Target Substitute;
    static inline Target Chain[nMaxChain];

    static inline ID3D10VertexShader* pVertexShader = nullptr;
    static inline ID3D10PixelShader* pBlitShader = nullptr;
    static inline ID3D10SamplerState* pPointSampler = nullptr;
    static inline ID3D10SamplerState* pLinearSampler = nullptr;
    static inline bool bSetupFailed = false;

    static bool Compile(const char* szSource, const char* szEntry, const char* szProfile,
                        std::vector<uint8_t>& vOut)
    {
        // The renderer refuses to initialise without d3d10.dll, so the compiler comes out of the
        // module the game itself is already using.
        auto hModule = GetModuleHandleW(L"d3d10.dll");
        if (!hModule)
            return false;

        auto pCompile = reinterpret_cast<D3D10CompileShader_t>(GetProcAddress(hModule, "D3D10CompileShader"));
        if (!pCompile)
            return false;

        ID3D10Blob* pShader = nullptr;
        ID3D10Blob* pErrors = nullptr;

        const auto hr = pCompile(szSource, strlen(szSource), nullptr, nullptr, nullptr,
                                 szEntry, szProfile, 0, &pShader, &pErrors);

        if (SUCCEEDED(hr) && pShader)
        {
            const auto* pStart = static_cast<const uint8_t*>(pShader->GetBufferPointer());
            vOut.assign(pStart, pStart + pShader->GetBufferSize());
        }

        SafeRelease(pErrors);
        SafeRelease(pShader);
        return !vOut.empty();
    }

    static bool EnsureSetup(ID3D10Device* pDevice)
    {
        if (pVertexShader && pBlitShader && pPointSampler && pLinearSampler)
            return true;

        if (std::exchange(bSetupFailed, true))
            return false;

        std::vector<uint8_t> vCode;
        if (!Compile(szVertexShader, "ResolveVS", "vs_4_0", vCode) ||
            FAILED(pDevice->CreateVertexShader(vCode.data(), vCode.size(), &pVertexShader)))
            return false;

        vCode.clear();
        if (!Compile(szBlitShader, "ResolvePS", "ps_4_0", vCode) ||
            FAILED(pDevice->CreatePixelShader(vCode.data(), vCode.size(), &pBlitShader)))
            return false;

        D3D10_SAMPLER_DESC sampler{};
        sampler.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D10_FLOAT32_MAX;

        sampler.Filter = D3D10_FILTER_MIN_MAG_MIP_POINT;
        if (FAILED(pDevice->CreateSamplerState(&sampler, &pPointSampler)))
            return false;

        sampler.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
        if (FAILED(pDevice->CreateSamplerState(&sampler, &pLinearSampler)))
            return false;

        bSetupFailed = false;
        return true;
    }

    // One textured triangle from pSrc into a rect of pDst. The rect is the whole target for every
    // stage of the halving chain and, for the final pass, the aspect-preserving fit inside the
    // backbuffer, so bars are the only thing that ever separates the two.
    static void Blit(ID3D10Device* pDevice, ID3D10RenderTargetView* pDst,
                     int32_t nDstX, int32_t nDstY, UINT nDstW, UINT nDstH,
                     ID3D10ShaderResourceView* pSrc, int32_t eFilter)
    {
        pDevice->OMSetRenderTargets(1, &pDst, nullptr);

        D3D10_VIEWPORT viewport{};
        viewport.TopLeftX = nDstX;
        viewport.TopLeftY = nDstY;
        viewport.Width = nDstW;
        viewport.Height = nDstH;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        pDevice->RSSetViewports(1, &viewport);

        // Defaults for all three: opaque, no depth or stencil, solid fill with no scissor. The
        // engine's own values come back next frame out of the state shadow FUN_10442A90 zeroes
        // immediately after Present.
        const float fBlend[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        pDevice->OMSetBlendState(nullptr, fBlend, 0xFFFFFFFF);
        pDevice->OMSetDepthStencilState(nullptr, 0);
        pDevice->RSSetState(nullptr);

        ID3D10Buffer* pNullBuffer = nullptr;
        UINT nZero = 0;
        pDevice->IASetInputLayout(nullptr);
        pDevice->IASetVertexBuffers(0, 1, &pNullBuffer, &nZero, &nZero);
        pDevice->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        pDevice->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        pDevice->VSSetShader(pVertexShader);
        pDevice->GSSetShader(nullptr);
        pDevice->PSSetShader(pBlitShader);
        pDevice->PSSetShaderResources(0, 1, &pSrc);

        auto* pSampler = (eFilter == FILTER_POINT) ? pPointSampler : pLinearSampler;
        pDevice->PSSetSamplers(0, 1, &pSampler);

        pDevice->Draw(3, 0);

        // Never leave the source bound: the next stage of the chain renders into it.
        ID3D10ShaderResourceView* pNullSrv = nullptr;
        pDevice->PSSetShaderResources(0, 1, &pNullSrv);
    }

public:
    static ID3D10Texture2D* Texture() { return Substitute.pTex; }

    static bool Ensure(ID3D10Device* pDevice, UINT w, UINT h, DXGI_FORMAT fmt)
    {
        return pDevice && EnsureSetup(pDevice) && Substitute.Ensure(pDevice, w, h, fmt);
    }

    static void Resolve(ID3D10Device* pDevice, IDXGISwapChain* pSwapChain)
    {
        if (!pDevice || !pSwapChain || !Substitute.pSrv || !pVertexShader || !pBlitShader)
            return;

        // Taken fresh every frame rather than cached. A held reference to buffer zero makes the
        // next ResizeBuffers fail with DXGI_ERROR_INVALID_CALL.
        ID3D10Texture2D* pBackBuffer = nullptr;
        if (FAILED(pSwapChain->GetBuffer(0, __uuidof(ID3D10Texture2D),
                                         reinterpret_cast<void**>(&pBackBuffer))) || !pBackBuffer)
            return;

        D3D10_TEXTURE2D_DESC desc{};
        pBackBuffer->GetDesc(&desc);

        ID3D10RenderTargetView* pTargetView = nullptr;
        if (FAILED(pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pTargetView)) || !pTargetView)
        {
            SafeRelease(pBackBuffer);
            return;
        }

        // The largest rect of the backbuffer that still has the internal frame's shape, centred,
        // as above.
        UINT nFitW = desc.Width;
        UINT nFitH = desc.Height;
        {
            const auto w = static_cast<uint64_t>(Substitute.nWidth);
            const auto h = static_cast<uint64_t>(Substitute.nHeight);

            if (w * desc.Height > h * desc.Width)
                nFitH = (std::max)(1u, static_cast<UINT>((desc.Width * h + w / 2) / w));
            else if (h * desc.Width > w * desc.Height)
                nFitW = (std::max)(1u, static_cast<UINT>((desc.Height * w + h / 2) / h));
        }

        const auto nFitX = static_cast<int32_t>((desc.Width - nFitW) / 2);
        const auto nFitY = static_cast<int32_t>((desc.Height - nFitH) / 2);

        auto* pSrc = Substitute.pSrv;
        UINT nSrcW = Substitute.nWidth;
        UINT nSrcH = Substitute.nHeight;

        for (size_t stage = 0; stage < nMaxChain; ++stage)
        {
            if (nSrcW < nFitW * 2 && nSrcH < nFitH * 2)
                break;

            const UINT dw = (nSrcW >= nFitW * 2) ? (std::max)(nSrcW / 2, nFitW) : nSrcW;
            const UINT dh = (nSrcH >= nFitH * 2) ? (std::max)(nSrcH / 2, nFitH) : nSrcH;

            if (!Chain[stage].Ensure(pDevice, dw, dh, Substitute.eFormat))
                break;

            Blit(pDevice, Chain[stage].pRtv, 0, 0, dw, dh, pSrc, FILTER_BILINEAR);

            pSrc = Chain[stage].pSrv;
            nSrcW = dw;
            nSrcH = dh;
        }

        // Only when there is something outside the image. The swapchain is
        // DXGI_SWAP_EFFECT_DISCARD, so the bars hold whatever the driver last left there.
        if (nFitW != desc.Width || nFitH != desc.Height)
        {
            const float fBlack[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            pDevice->ClearRenderTargetView(pTargetView, fBlack);
        }

        Blit(pDevice, pTargetView, nFitX, nFitY, nFitW, nFitH, pSrc, nFilter);

        // The engine re-issues everything else next frame from a state shadow that is zeroed right
        // after Present. These two are the exception: where its next wanted value is also null it
        // compares equal to the zeroed shadow and skips the call, leaving this binding live.
        ID3D10ShaderResourceView* pNullSrv = nullptr;
        ID3D10SamplerState* pNullSampler = nullptr;
        pDevice->PSSetShaderResources(0, 1, &pNullSrv);
        pDevice->PSSetSamplers(0, 1, &pNullSampler);

        SafeRelease(pTargetView);
        SafeRelease(pBackBuffer);
    }
};

// ---------------------------------------------------------------------------------------------
// Hooks

static ID3D10Device* Device(uintptr_t nRenderer)
{
    return *reinterpret_cast<ID3D10Device**>(nRenderer + nRendererDevice);
}

static IDXGISwapChain* SwapChain(uintptr_t nRenderer)
{
    return *reinterpret_cast<IDXGISwapChain**>(nRenderer + nRendererSwapChain);
}

// Decides, once per swapchain, whether the split is on and at what pair of sizes. Runs at the entry
// of FUN_1041FE80, the only place the backbuffer is fetched, which is reached after creation as
// well as after a resize.
static void PrepareSplit(uintptr_t nRenderer)
{
    bActive = false;

    auto* pDevice = Device(nRenderer);
    auto* pSwapChain = SwapChain(nRenderer);
    if (!pDevice || !pSwapChain)
        return;

    DXGI_SWAP_CHAIN_DESC scd{};
    if (FAILED(pSwapChain->GetDesc(&scd)))
        return;

    const bool bWindowed = *reinterpret_cast<uint32_t*>(nRenderer + nDescWindowed) != 0;

    if (bWindowed)
    {
        // What DXGI stretches the backbuffer to, which is the window whatever the video options
        // say once borderless has stopped the window following them.
        const auto hWnd = *reinterpret_cast<HWND*>(nRenderer + nDescOutputWindow);
        RECT client{};
        if (!hWnd || !GetClientRect(hWnd, &client))
            return;

        nOutputW = static_cast<uint32_t>(client.right - client.left);
        nOutputH = static_cast<uint32_t>(client.bottom - client.top);
    }
    else
    {
        // Exclusive fullscreen owns the display mode and the buffers are already it, so the
        // swapchain is the authority rather than the window. Read from the description instead of
        // measuring, because the two disagree for a frame across a mode transition.
        nOutputW = scd.BufferDesc.Width;
        nOutputH = scd.BufferDesc.Height;
    }

    if (nOutputW == 0 || nOutputH == 0)
        return;

    if (nRequestedW > 0 && nRequestedH > 0)
    {
        nInternalW = static_cast<uint32_t>(nRequestedW);
        nInternalH = static_cast<uint32_t>(nRequestedH);
    }
    else if (bBorderless)
    {
        // The resolution the video options asked for, read from the pair FUN_1041FB00 writes out
        // of SetMode's arguments rather than from the swapchain description, which EndFrame
        // rewrites from the client rect whenever it rebuilds.
        nInternalW = *reinterpret_cast<uint32_t*>(nRenderer + nRendererModeWidth);
        nInternalH = *reinterpret_cast<uint32_t*>(nRenderer + nRendererModeHeight);
    }
    else
    {
        return;
    }

    if (nInternalW == 0 || nInternalH == 0)
        return;

    // Nothing to split while the two still agree, so borderless at the desktop resolution and a
    // matched internal resolution both stay byte for byte stock.
    if (nInternalW == nOutputW && nInternalH == nOutputH)
        return;

    // The buffers the engine is about to fetch have to be the size that reaches the display, not
    // the size it draws at. BufferCount zero and format UNKNOWN keep the description's own values.
    // Fullscreen never needs it, having read both from the same description. A resize there would
    // be a display mode change nobody asked for, so a disagreement declines instead.
    if (scd.BufferDesc.Width != nOutputW || scd.BufferDesc.Height != nOutputH)
    {
        if (!bWindowed)
            return;

        if (FAILED(pSwapChain->ResizeBuffers(0, nOutputW, nOutputH, DXGI_FORMAT_UNKNOWN, scd.Flags)))
            return;
    }

    if (!CResolver::Ensure(pDevice, nInternalW, nInternalH, scd.BufferDesc.Format))
        return;

    bActive = true;
}

// Renderer vtable+0x44, the size the engine allocates from.
static SafetyHookInline BackBufferSizeHook{};

static void __fastcall BackBufferSize(uint8_t* pRenderer, void* pEdx, HWND hWnd,
                                      int32_t* pWidth, int32_t* pHeight)
{
    // EndFrame refills the swapchain description through this same call and wants the real thing.
    // It is the only caller that passes the description's own fields as the out-pointers, which is
    // cheaper to recognise than the return address and does not care how it was reached.
    const bool bDescription = pWidth == reinterpret_cast<int32_t*>(pRenderer + nDescWidth);

    if (bActive && !bDescription && pWidth && pHeight)
    {
        *pWidth = static_cast<int32_t>(nInternalW);
        *pHeight = static_cast<int32_t>(nInternalH);
        return;
    }

    BackBufferSizeHook.fastcall(pRenderer, pEdx, hWnd, pWidth, pHeight);
}

class InternalResolutionDX10
{
public:
    InternalResolutionDX10()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // FUN_1041FE80, matched from its prologue through the swapchain null test.
            auto backBufferViews = dunia_pattern("51 56 8B F1 8B 46 40 57 33 FF 3B C7 0F 84");

            // The same function eighteen bytes in, where the description's width and height are
            // already in EBX and EBP on their way to SetViewport and dead everywhere else.
            auto viewport = dunia_pattern("53 8B 5E 4C 55 8B 6E 50 8D 54 24 10 52 68 ? ? ? ?");

            // Its call to the wrapper's texture setter, with the backbuffer texture in ECX one
            // instruction from being pushed. Anchored on the push rather than the load above it,
            // because the load would be relocated over the top of anything written to ECX.
            auto attachTexture = dunia_pattern("51 8B CF E8 ? ? ? ? 6A 00 57 E8");

            // Renderer vtable+0x44, from its prologue through the windowed test and the
            // GetClientRect that test guards.
            auto backBufferSize = dunia_pattern("83 EC 10 83 79 7C 00 74 76 8B 4C 24 14 8D 04 24 50 51 FF 15 ? ? ? ?");

            // The end of the frame function, after everything the engine draws and before the
            // Present argument loads.
            auto frameEnd = dunia_pattern("BD 01 00 00 00 55 6A 02 E8 ? ? ? ? 8B 46 40 8B 96 8C 00 00 00");

            if (backBufferViews.empty() || viewport.empty() || attachTexture.empty() ||
                backBufferSize.empty() || frameEnd.empty())
                return;

            BackBufferSizeHook = safetyhook::create_inline(backBufferSize.get_first(), BackBufferSize);

            static auto BackBufferViewsHook = safetyhook::create_mid(backBufferViews.get_first(), [](SafetyHookContext& regs)
            {
                // ECX is the renderer; the relocated MOV copies it into ESI a moment from now.
                PrepareSplit(regs.ecx);
            });

            static auto ViewportHook = safetyhook::create_mid(viewport.get_first(0x08), [](SafetyHookContext& regs)
            {
                if (!bActive)
                    return;

                regs.ebx = nInternalW;
                regs.ebp = nInternalH;
            });

            static auto AttachTextureHook = safetyhook::create_mid(attachTexture.get_first(), [](SafetyHookContext& regs)
            {
                if (!bActive)
                    return;

                if (auto* pTexture = CResolver::Texture())
                    regs.ecx = reinterpret_cast<uintptr_t>(pTexture);
            });

            static auto FrameEndHook = safetyhook::create_mid(frameEnd.get_first(), [](SafetyHookContext& regs)
            {
                if (bActive)
                    CResolver::Resolve(Device(regs.esi), SwapChain(regs.esi));
            });

            JackalFix::onShutdownEvent() += []()
            {
                bActive = false;
            };
        };
    }
} InternalResolutionDX10;

} // namespace dx10

// ---------------------------------------------------------------------------------------------
// What shape the frame the engine composes into actually is
//
// hudfixes needs it and nothing else can answer. The HUD is drawn into the internal frame along
// with the scene and the frame is then fitted to the output whole, so the shape the player sees
// the HUD at is the internal frame's and not the window's. The engine's own display descriptor
// describes the window, which is the same answer only when the two shapes agree.
//
// Whichever backend is live has filled its own pair and the other's stays zero. Both stay zero
// until the first device is made, and false then means the caller should fall back to the
// descriptor rather than assume anything.
export bool GetInternalFrameSize(uint32_t& nWidth, uint32_t& nHeight)
{
    if (::nInternalW != 0 && ::nInternalH != 0)
    {
        nWidth = ::nInternalW;
        nHeight = ::nInternalH;
        return true;
    }

    if (dx10::nInternalW != 0 && dx10::nInternalH != 0)
    {
        nWidth = dx10::nInternalW;
        nHeight = dx10::nInternalH;
        return true;
    }

    return false;
}
