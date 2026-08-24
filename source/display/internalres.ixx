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

export module internalres;

import common;
import dunia;
import settings;

template<typename T>
static void SafeRelease(T*& ptr)
{
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

enum ScalingFilter
{
    FILTER_POINT = 0,
    FILTER_BILINEAR = 1,
};

static int32_t nRequestedW = 0;         // from the ini; 0 leaves the game alone
static int32_t nRequestedH = 0;
static int32_t nFilter = FILTER_BILINEAR;
static bool bBorderless = false;        // borderless splits with no ini request

// Viewport, scene targets, depth and the final PostFxBlit all follow the internal size, so nothing
// needs rescaling.
static uint32_t nInternalW = 0;         // what the engine believes the screen is
static uint32_t nInternalH = 0;
static uint32_t nOutputW = 0;           // what the swapchain presents
static uint32_t nOutputH = 0;

// Last surface the device confirmed as its backbuffer; identity is re-established from the device
// whenever an unfamiliar surface turns up.
static IDirect3DSurface9* pKnownBackBuffer = nullptr;
static bool bSubstituted = false;

// The present-parameter globals, from the pattern's own operands; ASLR moves them.
static uint32_t* pBackBufferWidth = nullptr;
static uint32_t* pBackBufferHeight = nullptr;

// The resolve pass

class CResolver
{
    struct ScreenVertex { float x, y, z, rhw, u, v; };
    static constexpr DWORD nScreenFVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

    // Four halvings covers 16x per axis.
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
    // One textured quad pSrc into a rect of pDst: whole target for each halving stage, the
    // aspect-preserving fit inside the backbuffer for the final pass.
    static bool Blit(IDirect3DDevice9* pDevice, IDirect3DSurface9* pDst, UINT nTargetW, UINT nTargetH,
                     int32_t nDstX, int32_t nDstY, UINT nDstW, UINT nDstH,
                     IDirect3DTexture9* pSrc, int32_t eFilter)
    {
        // Depth first: D3D9 rejects a render target smaller than the bound depth-stencil, and while
        // supersampling the engine's depth buffer is the larger.
        pDevice->SetDepthStencilSurface(nullptr);

        if (FAILED(pDevice->SetRenderTarget(0, pDst)))
            return false;

        // Viewport is the whole target, not the fitted rect, so the clear reaches the bars.
        const D3DVIEWPORT9 viewport = { 0, 0, nTargetW, nTargetH, 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);

        // D3DSWAPEFFECT_DISCARD: the bars hold stale driver content unless written every frame.
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
        // Frame is already in the space the game finished in; converting would shift every pixel.
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

        // Wrapping would make the 0-to-1 texcoord take the short way round; state block restores.
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

    // Top of the frame function, before the screenshot block and Present. Present is outside the
    // scene bracket (no DAT_11609D78 write inside 0x10424150), so draw work here is legal.
    static void Resolve(IDirect3DDevice9* pDevice)
    {
        if (!pDevice || !Substitute.pSurf)
            return;

        IDirect3DSurface9* pBackBuffer = nullptr;
        if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) || !pBackBuffer)
            return;

        // Matching sizes are no reason to skip the copy: the engine drew into the substitute.
        D3DSURFACE_DESC desc{};
        pBackBuffer->GetDesc(&desc);

        // Largest backbuffer rect with the internal frame's shape, centred: 4:3 on 16:9 gets bars,
        // not a stretch. From the substitute, not the chain's output, since the halvings round.
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

        // Nothing else puts a frame in front of the player, so no path out may leave the backbuffer
        // untouched. StretchRect is the fallback: driver's filter, but it needs no device state.
        if (!pStateBlock && FAILED(pDevice->CreateStateBlock(D3DSBT_ALL, &pStateBlock)))
        {
            // ColorFill, not Clear: no device state can be saved and restored on this path.
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

        // A bilinear tap is an exact 2x2 box average only at half size, so anything past 2:1 is
        // halved through scratch targets first. The chain targets the fitted rect.
        for (size_t stage = 0; stage < nMaxChain; ++stage)
        {
            // Either axis at 2:1 is enough; requiring both leaves a one-axis reduction on four taps.
            if (nSrcW < nFitW * 2 && nSrcH < nFitH * 2)
                break;

            const UINT dw = (nSrcW >= nFitW * 2) ? (std::max)(nSrcW / 2, nFitW) : nSrcW;
            const UINT dh = (nSrcH >= nFitH * 2) ? (std::max)(nSrcH / 2, nFitH) : nSrcH;

            if (!Chain[stage].Ensure(pDevice, dw, dh, Substitute.eFormat))
                break;

            // A stage that did not run still holds the previous frame.
            if (!Blit(pDevice, Chain[stage].pSurf, dw, dh, 0, 0, dw, dh, pSrc, FILTER_BILINEAR))
                break;

            pSrc = Chain[stage].pTex;
            nSrcW = dw;
            nSrcH = dh;
        }

        Blit(pDevice, pBackBuffer, desc.Width, desc.Height, nFitX, nFitY, nFitW, nFitH, pSrc, nFilter);

        if (bOpenedScene)
            pDevice->EndScene();

        // Both or neither: slot 0 can never be unbound, and the engine's larger depth buffer would
        // be rejected against an output-sized backbuffer, costing the rest of the frame its depth.
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

    // Entry of the pre-reset release pass. Every D3DPOOL_DEFAULT resource must be gone before Reset,
    // and the render-target binding holds a reference: left bound, Reset returns D3DERR_INVALIDCALL.
    static void ReleaseForReset(IDirect3DDevice9* pDevice)
    {
        pKnownBackBuffer = nullptr;

        if (pDevice)
        {
            // Depth first, as in Blit.
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

        // Only this module's own two references; the engine's Release just below takes the last.
        Substitute.Release();

        SafeRelease(pStateBlock);
    }

    // Process teardown. The device may already be gone.
    static void ReleaseAll()
    {
        pKnownBackBuffer = nullptr;

        for (auto& target : Chain)
            target.Release();

        Substitute.Release();
        SafeRelease(pStateBlock);
    }
};

// Hook bodies

// Renderer vtable +0x40, end of frame. `this` in ECX, renderer+0x38 the IDirect3DDevice9*.
static void OnFrameEnd(uintptr_t nRenderer)
{
    if (!bSubstituted || !nRenderer)
        return;

    CResolver::Resolve(*reinterpret_cast<IDirect3DDevice9**>(nRenderer + 0x38));
}

// FUN_1040F190, the wrapper's generic surface setter: every render-target wrapper goes through it,
// so the backbuffer is recognised, not assumed. Tested on the surface, so a rebuilt device is fine.
static void OnSetWrapperSurface(IDirect3DSurface9** ppSurface)
{
    if (nInternalW == 0 || nInternalH == 0 || nOutputW == 0 || nOutputH == 0)
        return;

    // Nothing to gain; substituting would only cost a copy.
    if (nInternalW == nOutputW && nInternalH == nOutputH)
        return;

    IDirect3DSurface9* pIncoming = *ppSurface;

    // The wrapper is torn down and set to NULL twice a frame.
    if (!pIncoming)
        return;

    // Common case costs a pointer compare; identity is re-established only when there is none.
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

        // Replaces the GetBackBuffer reference: the wrapper owns exactly one and releases it next
        // frame.
        pSubstitute->AddRef();
        *ppSurface = pSubstitute;
        pIncoming->Release();

        bSubstituted = true;
    }
    else
    {
        // Engine keeps the real backbuffer with internal-sized targets: wrong but visible beats
        // resolving against a target that no longer exists.
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
            // FUN_10423400. Width and height arrive by pointer into caller stack slots read back at
            // 0x10423610, so writing them here reaches viewport, renderer fields, aspect and params.
            auto setResolution = dunia_pattern("81 EC 48 02 00 00 53 55 56 57 33 ED 55 8D 84 24 B4 00 00 00");
            if (setResolution.empty())
                return;

            // The only writers of those globals; hooked past rather than replaced.
            auto presentParams = dunia_pattern("8B 56 1C 89 15 ? ? ? ? 8B 46 20 A3 ? ? ? ? 8A 4D 08 F6 D9");
            if (presentParams.empty())
                return;

            // FUN_1040F190. Pattern runs into the CC padding on purpose: an identical setter body
            // at 0x102795C4 is unpadded.
            auto wrapperSetter = dunia_pattern("8B 44 24 04 89 41 1C C2 04 00 CC CC CC CC CC CC");
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

                // Borderless has already split frame from output; routing it here gets the fit and
                // the filter instead of the runtime's point sample at the wrong shape.
                if ((nWantW <= 0 || nWantH <= 0) && bBorderless)
                {
                    // Only once the two differ: at the desktop resolution the split costs a
                    // fullscreen blit per frame for nothing. Measured, not assumed.
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

                // Present at what the engine was about to render at; windowed, replaced with the
                // measured client rect once the branch is known.
                nOutputW = *pWidth;
                nOutputH = *pHeight;

                if (!bWanted || nOutputW == 0 || nOutputH == 0)
                    return;

                // "Use the desktop resolution" makes GetScreenSize return the client rect instead of
                // the stored pair, undoing the split. Clearing it costs nothing.
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

                // EBP is the device config, ESI the renderer. Hooked on the fullscreen-byte read;
                // DAT_11609D40 (Windowed) came from the same byte, so one read answers both.
                auto* pConfig = reinterpret_cast<uint8_t*>(regs.ebp);
                const bool bFullscreen = pConfig[8] != 0;

                if (!bFullscreen)
                {
                    // Measure, not recompute, so backbuffer and client area cannot drift apart.
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

                // Letterboxed, so the engine must compose for the internal frame's shape. Fullscreen
                // only; left alone when the shapes agree, so forced-aspect still holds.
                const auto fInternal = static_cast<float>(nInternalW) / static_cast<float>(nInternalH);
                const auto fOutput = static_cast<float>(nOutputW) / static_cast<float>(nOutputH);

                if (bFullscreen && std::fabs(fInternal - fOutput) > 0.0001f)
                    *reinterpret_cast<float*>(regs.esi + 0x24) = fInternal;
            });

            static auto WrapperSetterHook = safetyhook::create_mid(wrapperSetter.get_first(), [](SafetyHookContext& regs)
            {
                // __thiscall f(this in ECX, surface at [esp+4]), callee-cleaned; the relocated
                // MOV EAX,[ESP+4] reads the slot back.
                OnSetWrapperSurface(reinterpret_cast<IDirect3DSurface9**>(regs.esp + 0x04));
            });

            static auto FrameEndHook = safetyhook::create_mid(frameEnd.get_first(), [](SafetyHookContext& regs)
            {
                OnFrameEnd(regs.ecx);
            });

            static auto PreResetHook = safetyhook::create_mid(preReset.get_first(), [](SafetyHookContext& regs)
            {
                // Unconditional: nothing here may outlive the Reset four instructions away.
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

            // An ini change lands on the next launch or resolution change.
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

// Direct3D 10
//
// The same split against the other renderer; nothing above this line runs here. Where each size
// lives:
//
//   renderer+0x1C/+0x20    the resolution the video options asked for.
//   vtable+0x44            the size the engine allocates from. Answered with the internal
//                          resolution, except for EndFrame, which wants the real size.
//   the swapchain buffers  resized to the client area, windowed and borderless only.
//   the render target      substituted where the backbuffer texture is attached to its wrapper.
//
// The viewport comes from the swapchain description, so it is overridden separately. The resolve
// runs after the frame is drawn and before Present.

namespace dx10
{

// Renderer fields off DAT_11609668; the D3D9 renderer shares the global and the vtable layout.
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

// Three vertices from SV_VertexID: no buffer, input layout or declaration needed. The viewport is
// the fitted rect, so the triangle covers exactly the frame's area.
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
            // Both: the wrapper builds views from these and the resolve reads back what was drawn.
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
        // The compiler comes out of d3d10.dll, which the renderer will not initialise without.
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

    // As the D3D9 Blit, with a full-screen triangle.
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

        // Defaults: opaque, no depth or stencil, solid fill, no scissor. The engine's values come
        // back from the state shadow FUN_10442A90 zeroes right after Present.
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

        // The next stage of the chain renders into the source.
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

        // Fetched fresh: a held reference to buffer zero fails the next ResizeBuffers.
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

        // Largest backbuffer rect with the internal frame's shape, centred, as above.
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

        // DXGI_SWAP_EFFECT_DISCARD: the bars hold stale driver content unless written.
        if (nFitW != desc.Width || nFitH != desc.Height)
        {
            const float fBlack[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            pDevice->ClearRenderTargetView(pTargetView, fBlack);
        }

        Blit(pDevice, pTargetView, nFitX, nFitY, nFitW, nFitH, pSrc, nFilter);

        // Exceptions to the state shadow: next wanted value is also null, so the call is skipped
        // and the binding would stay live.
        ID3D10ShaderResourceView* pNullSrv = nullptr;
        ID3D10SamplerState* pNullSampler = nullptr;
        pDevice->PSSetShaderResources(0, 1, &pNullSrv);
        pDevice->PSSetSamplers(0, 1, &pNullSampler);

        SafeRelease(pTargetView);
        SafeRelease(pBackBuffer);
    }
};

static ID3D10Device* Device(uintptr_t nRenderer)
{
    return *reinterpret_cast<ID3D10Device**>(nRenderer + nRendererDevice);
}

static IDXGISwapChain* SwapChain(uintptr_t nRenderer)
{
    return *reinterpret_cast<IDXGISwapChain**>(nRenderer + nRendererSwapChain);
}

// Once per swapchain: is the split on, and at what sizes. Entry of FUN_1041FE80, the only
// backbuffer fetch, reached after creation and after resize.
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
        // What DXGI stretches the backbuffer to: the window, not the video options.
        const auto hWnd = *reinterpret_cast<HWND*>(nRenderer + nDescOutputWindow);
        RECT client{};
        if (!hWnd || !GetClientRect(hWnd, &client))
            return;

        nOutputW = static_cast<uint32_t>(client.right - client.left);
        nOutputH = static_cast<uint32_t>(client.bottom - client.top);
    }
    else
    {
        // Exclusive fullscreen: the swapchain is the authority, not the window, which disagrees for
        // a frame across a mode transition.
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
        // From the pair FUN_1041FB00 writes out of SetMode's arguments, not the swapchain
        // description, which EndFrame rewrites from the client rect.
        nInternalW = *reinterpret_cast<uint32_t*>(nRenderer + nRendererModeWidth);
        nInternalH = *reinterpret_cast<uint32_t*>(nRenderer + nRendererModeHeight);
    }
    else
    {
        return;
    }

    if (nInternalW == 0 || nInternalH == 0)
        return;

    // Nothing to split while the two agree.
    if (nInternalW == nOutputW && nInternalH == nOutputH)
        return;

    // Buffers must be the size that reaches the display. BufferCount 0 and format UNKNOWN keep the
    // description's values; a fullscreen mismatch would mean an unasked-for mode change, so decline.
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
    // EndFrame refills the swapchain description through this call and wants the real size; it is
    // the only caller passing the description's own fields as the out-pointers.
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

            // The same function eighteen bytes in: description width and height in EBX and EBP on
            // their way to SetViewport.
            auto viewport = dunia_pattern("53 8B 5E 4C 55 8B 6E 50 8D 54 24 10 52 68 ? ? ? ?");

            // Its call to the wrapper's texture setter, backbuffer texture in ECX. Anchored on the
            // push: the load above would be relocated over ECX.
            auto attachTexture = dunia_pattern("51 8B CF E8 ? ? ? ? 6A 00 57 E8");

            // Renderer vtable+0x44, prologue through the windowed test and its GetClientRect.
            auto backBufferSize = dunia_pattern("83 EC 10 83 79 7C 00 74 76 8B 4C 24 14 8D 04 24 50 51 FF 15 ? ? ? ?");

            // The end of the frame function, after the drawing and before the Present argument loads.
            auto frameEnd = dunia_pattern("BD 01 00 00 00 55 6A 02 E8 ? ? ? ? 8B 46 40 8B 96 8C 00 00 00");

            if (backBufferViews.empty() || viewport.empty() || attachTexture.empty() ||
                backBufferSize.empty() || frameEnd.empty())
                return;

            BackBufferSizeHook = safetyhook::create_inline(backBufferSize.get_first(), BackBufferSize);

            static auto BackBufferViewsHook = safetyhook::create_mid(backBufferViews.get_first(), [](SafetyHookContext& regs)
            {
                // ECX is the renderer.
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

// The shape the engine composes into, for hudfixes: the HUD is fitted with the scene, so the
// internal frame's shape wins over the descriptor's. Both pairs are zero until the first device.
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
