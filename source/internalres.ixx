module;

#include <common.hxx>
#include <algorithm>
#include <utility>
#include <vector>
#include <cstdint>
#include <d3d9.h>

// Internal render resolution independent of the window, display mode and backbuffer.
//
// FUN_10423400 (SetResolution) stores a width/height pair at renderer+0x1C/+0x20, derives the
// aspect into renderer+0x24, and copies the same pair into the global D3DPRESENT_PARAMETERS as
// BackBufferWidth/BackBufferHeight. Split: renderer+0x1C/+0x20 becomes the internal resolution,
// which the viewport, scene targets, shared depth buffer and final PostFxBlit all size off.
// BackBufferWidth/Height stays the real output size: the window's client area when windowed or
// borderless, the adapter mode when fullscreen.
//
// That leaves the backbuffer the wrong size, so it is substituted. The engine reaches it through a
// 0x20-byte wrapper whose only setter is FUN_1040F190 and which caches no dimensions: vtable+0x10
// calls the surface's GetDesc live. The frame function resolves that target onto the real
// backbuffer before the engine's screenshot block and Present. The resolve exists because D3D9's
// own stretch of a mismatched backbuffer at Present is a point sample. It picks its own filter, and
// halves through scratch targets above 2:1 because a bilinear tap is an exact 2x2 box average only
// at exactly half size. Scale is uniform, so a mismatched shape gets bars.
//
// Exclusive fullscreen needs nothing special: that branch of SetResolution already leaves
// renderer+0x90 clear, and the resolution reaching it has been rounded to a real adapter mode by
// the boot path. The engine's screenshot key sizes from renderer+0x1C/+0x20, so its shots come out
// at the internal resolution.

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
    FILTER_CATMULLROM = 2,
};

static int32_t nRequestedW = 0;         // from the ini; 0 disables
static int32_t nRequestedH = 0;
static int32_t nFilter = FILTER_BILINEAR;

static uint32_t nInternalW = 0;         // what the engine believes the screen is
static uint32_t nInternalH = 0;
static uint32_t nOutputW = 0;           // what the swapchain actually presents
static uint32_t nOutputH = 0;

// The fullscreen byte is read once, at the present-parameter write, and answers two questions:
// which of SetResolution's three branches computed the aspect, and whether the device is about to
// be windowed. Only works because DisplayMode settles the byte before the branch reads it. The old
// Borderless option cleared it after the branch instead, which made the two readings disagree and
// left a windowed device carrying a fullscreen aspect. Ordering shared with borderless.ixx.

// Last surface the device confirmed as its backbuffer. Fast path; re-established from the device
// whenever an unfamiliar surface turns up.
static IDirect3DSurface9* pKnownBackBuffer = nullptr;
static bool bSubstituted = false;

// The two present-parameter globals, read out of the pattern's own operands since ASLR moves them.
static uint32_t* pBackBufferWidth = nullptr;
static uint32_t* pBackBufferHeight = nullptr;

// ---------------------------------------------------------------------------------------------
// The resolve pass

// Catmull-Rom kernel for the final pass. Only used scaling up; going down, the halving chain has
// already box-filtered.
static constexpr const char* szCatmullRomPS = R"(
sampler2D SrcTex : register(s0);
float4 SrcSize : register(c0);   // xy = 1 / source size, zw = source size

float4 CatmullRomPS(in float2 uv : TEXCOORD0) : COLOR0
{
    float2 samplePos = uv * SrcSize.zw;
    float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;

    float2 texPos0  = (texPos1 - 1.0f)     * SrcSize.xy;
    float2 texPos3  = (texPos1 + 2.0f)     * SrcSize.xy;
    float2 texPos12 = (texPos1 + offset12) * SrcSize.xy;

    float4 result = 0.0f;
    result += tex2D(SrcTex, float2(texPos0.x,  texPos0.y))  * w0.x  * w0.y;
    result += tex2D(SrcTex, float2(texPos12.x, texPos0.y))  * w12.x * w0.y;
    result += tex2D(SrcTex, float2(texPos3.x,  texPos0.y))  * w3.x  * w0.y;

    result += tex2D(SrcTex, float2(texPos0.x,  texPos12.y)) * w0.x  * w12.y;
    result += tex2D(SrcTex, float2(texPos12.x, texPos12.y)) * w12.x * w12.y;
    result += tex2D(SrcTex, float2(texPos3.x,  texPos12.y)) * w3.x  * w12.y;

    result += tex2D(SrcTex, float2(texPos0.x,  texPos3.y))  * w0.x  * w3.y;
    result += tex2D(SrcTex, float2(texPos12.x, texPos3.y))  * w12.x * w3.y;
    result += tex2D(SrcTex, float2(texPos3.x,  texPos3.y))  * w3.x  * w3.y;

    return float4(result.rgb, 1.0f);
}
)";

struct IShaderBlob : public IUnknown
{
    virtual void* STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};

using D3DXCompileShader_t = HRESULT(WINAPI*)(const char* pSrcData, UINT SrcDataLen, const void* pDefines,
    void* pInclude, const char* pFunctionName, const char* pProfile, DWORD Flags,
    IShaderBlob** ppShader, IShaderBlob** ppErrorMsgs, void** ppConstantTable);

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
    static inline IDirect3DPixelShader9* pCatmullRom = nullptr;
    // Shaders survive Reset but not a device that is destroyed and rebuilt, so the bytecode is
    // kept apart from the shader object and coming back costs only a CreatePixelShader.
    static inline std::vector<uint8_t> vBytecode;
    static inline bool bCompileFailed = false;

    static bool EnsureCatmullRom(IDirect3DDevice9* pDevice)
    {
        if (pCatmullRom)
            return true;

        if (!vBytecode.empty())
        {
            if (SUCCEEDED(pDevice->CreatePixelShader(reinterpret_cast<const DWORD*>(vBytecode.data()), &pCatmullRom)))
                return true;

            pCatmullRom = nullptr;
            return false;
        }

        if (std::exchange(bCompileFailed, true))
            return false;

        // Dunia imports d3dx9_38. The others cover installs that pulled in a different D3DX.
        D3DXCompileShader_t pCompile = nullptr;
        for (auto* szModule : { L"d3dx9_38.dll", L"d3dx9_43.dll", L"d3dx9_42.dll", L"d3dx9_41.dll" })
        {
            if (auto hModule = GetModuleHandleW(szModule))
            {
                pCompile = reinterpret_cast<D3DXCompileShader_t>(GetProcAddress(hModule, "D3DXCompileShader"));
                if (pCompile)
                    break;
            }
        }

        if (!pCompile)
            return false;

        // Nine dependent fetches plus the weight math fit ps_3_0 comfortably and ps_2_0 only just.
        for (auto* szProfile : { "ps_3_0", "ps_2_0" })
        {
            IShaderBlob* pShader = nullptr;
            IShaderBlob* pErrors = nullptr;

            const auto hr = pCompile(szCatmullRomPS, static_cast<UINT>(strlen(szCatmullRomPS)), nullptr,
                                     nullptr, "CatmullRomPS", szProfile, 0, &pShader, &pErrors, nullptr);

            if (SUCCEEDED(hr) && pShader)
            {
                const auto created = pDevice->CreatePixelShader(
                    reinterpret_cast<const DWORD*>(pShader->GetBufferPointer()), &pCatmullRom);

                if (SUCCEEDED(created) && pCatmullRom)
                {
                    const auto* pStart = static_cast<const uint8_t*>(pShader->GetBufferPointer());
                    vBytecode.assign(pStart, pStart + pShader->GetBufferSize());
                }

                SafeRelease(pErrors);
                SafeRelease(pShader);

                if (pCatmullRom)
                {
                    bCompileFailed = false;
                    return true;
                }
                continue;
            }

            SafeRelease(pErrors);
            SafeRelease(pShader);
        }

        return false;
    }

    // One textured quad from pSrc into a rect of pDst: the whole target for the halving chain,
    // the aspect-preserving fit for the final pass.
    static bool Blit(IDirect3DDevice9* pDevice, IDirect3DSurface9* pDst, UINT nTargetW, UINT nTargetH,
                     int32_t nDstX, int32_t nDstY, UINT nDstW, UINT nDstH,
                     IDirect3DTexture9* pSrc, UINT nSrcW, UINT nSrcH, int32_t eFilter)
    {
        // Depth first. D3D9 rejects a render target smaller than the bound depth-stencil, and the
        // engine's is larger than every target here while supersampling.
        pDevice->SetDepthStencilSurface(nullptr);

        if (FAILED(pDevice->SetRenderTarget(0, pDst)))
            return false;

        // Viewport is the whole target so the clear reaches the bars. The quad is positioned.
        const D3DVIEWPORT9 viewport = { 0, 0, nTargetW, nTargetH, 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);

        // Swapchain is D3DSWAPEFFECT_DISCARD, so bars hold stale driver contents unless rewritten.
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
        // The frame is already in the space the game finished in; converting would shift every
        // pixel.
        pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        for (DWORD i = 1; i < 8; ++i)
            pDevice->SetTexture(i, nullptr);

        const bool bUseShader = (eFilter == FILTER_CATMULLROM) && EnsureCatmullRom(pDevice);
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

        // A state block restores these afterwards but does not neutralise them first. Wrapping
        // makes the 0-to-1 texcoord take the short way round; the other two are vertex-stage
        // states, so they bite the shader path as well.
        pDevice->SetRenderState(D3DRS_WRAP0, 0);
        pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

        if (bUseShader)
        {
            const float consts[4] =
            {
                1.0f / static_cast<float>(nSrcW),
                1.0f / static_cast<float>(nSrcH),
                static_cast<float>(nSrcW),
                static_cast<float>(nSrcH),
            };
            pDevice->SetPixelShader(pCatmullRom);
            pDevice->SetPixelShaderConstantF(0, consts, 1);
        }
        else
        {
            // Fixed function, so point and bilinear need no shader and no D3DX at all.
            pDevice->SetPixelShader(nullptr);
            pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }

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

    // Top of the frame function, before the engine's screenshot block and Present. Present is
    // outside the scene bracket: nothing writes DAT_11609D78 inside 0x10424150.
    static void Resolve(IDirect3DDevice9* pDevice)
    {
        if (!pDevice || !Substitute.pSurf)
            return;

        IDirect3DSurface9* pBackBuffer = nullptr;
        if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) || !pBackBuffer)
            return;

        // No early-out on matching sizes: the substitute holds the only copy of the frame.
        D3DSURFACE_DESC desc{};
        pBackBuffer->GetDesc(&desc);

        // Largest rect of the backbuffer with the internal frame's shape, centred. Sized from the
        // substitute, since the halving stages round.
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

        // No path out of here may leave the backbuffer untouched, and StretchRect needs no state.
        if (!pStateBlock && FAILED(pDevice->CreateStateBlock(D3DSBT_ALL, &pStateBlock)))
        {
            // ColorFill rather than Clear: this path exists because no device state can be saved
            // and restored, and ColorFill needs none.
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
        // 2:1 halves through scratch targets first. Aimed at the fitted rect rather than the whole
        // backbuffer.
        for (size_t stage = 0; stage < nMaxChain; ++stage)
        {
            // Either axis at 2:1 is enough; requiring both would leave a frame oversampled on one
            // axis taking the whole reduction on four taps.
            if (nSrcW < nFitW * 2 && nSrcH < nFitH * 2)
                break;

            const UINT dw = (nSrcW >= nFitW * 2) ? (std::max)(nSrcW / 2, nFitW) : nSrcW;
            const UINT dh = (nSrcH >= nFitH * 2) ? (std::max)(nSrcH / 2, nFitH) : nSrcH;

            if (!Chain[stage].Ensure(pDevice, dw, dh, Substitute.eFormat))
                break;

            // A stage that did not run still holds the previous frame, so stop before sampling it.
            if (!Blit(pDevice, Chain[stage].pSurf, dw, dh, 0, 0, dw, dh, pSrc, nSrcW, nSrcH, FILTER_BILINEAR))
                break;

            pSrc = Chain[stage].pTex;
            nSrcW = dw;
            nSrcH = dh;
        }

        Blit(pDevice, pBackBuffer, desc.Width, desc.Height, nFitX, nFitY, nFitW, nFitH,
             pSrc, nSrcW, nSrcH, nFilter);

        if (bOpenedScene)
            pDevice->EndScene();

        // Both or neither. Slot 0 can never be unbound, so a failed GetRenderTarget leaves the
        // output-sized backbuffer bound and the engine's larger depth buffer rejected.
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

    // Entry of the engine's pre-reset release pass. Every D3DPOOL_DEFAULT resource must be gone
    // before Reset, and the device's render-target binding holds a reference to the substitute:
    // left bound, Reset returns D3DERR_INVALIDCALL, the device never comes back, and the screen
    // stays black for good after the first alt-tab or video-options change.
    //
    // The wrapper's reference is left in place on purpose. The engine dereferences wrapper+0x1C at
    // 0x1042471B with no null check and Releases it, which is what destroys the target.
    static void ReleaseForReset(IDirect3DDevice9* pDevice)
    {
        pKnownBackBuffer = nullptr;

        if (pDevice)
        {
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

        // This module's two references only. The wrapper keeps the third, released by the engine
        // four instructions from here.
        Substitute.Release();

        SafeRelease(pStateBlock);

        // Shaders survive Reset but not a device rebuild, and vBytecode is kept, so releasing
        // unconditionally costs at most a CreatePixelShader.
        SafeRelease(pCatmullRom);
    }

    // Process teardown; the device may already be gone, so nothing is asked of it.
    static void ReleaseAll()
    {
        pKnownBackBuffer = nullptr;

        for (auto& target : Chain)
            target.Release();

        Substitute.Release();
        SafeRelease(pStateBlock);
        SafeRelease(pCatmullRom);
    }
};

// ---------------------------------------------------------------------------------------------
// Hook bodies

// Renderer vtable +0x40, the end-of-frame function. `this` in ECX, renderer+0x38 is the device.
static void OnFrameEnd(uintptr_t nRenderer)
{
    if (!bSubstituted || !nRenderer)
        return;

    CResolver::Resolve(*reinterpret_cast<IDirect3DDevice9**>(nRenderer + 0x38));
}

// FUN_1040F190, the wrapper's surface setter. Every render-target wrapper goes through it, so the
// backbuffer has to be recognised. Testing the surface survives a device rebuild.
static void OnSetWrapperSurface(IDirect3DSurface9** ppSurface)
{
    if (nInternalW == 0 || nInternalH == 0 || nOutputW == 0 || nOutputH == 0)
        return;

    if (nInternalW == nOutputW && nInternalH == nOutputH)
        return;

    IDirect3DSurface9* pIncoming = *ppSurface;

    // The wrapper is torn down and set to NULL twice a frame.
    if (!pIncoming)
        return;

    // Common case is a pointer compare instead of two calls into the runtime. The identity is only
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

        // Stands in for the GetBackBuffer the engine just made. The wrapper owns one reference and
        // releases it unconditionally next frame, so replace it rather than add.
        pSubstitute->AddRef();
        *ppSurface = pSubstitute;
        pIncoming->Release();

        bSubstituted = true;
    }
    else
    {
        // Engine keeps the real backbuffer, so the frame is wrong. Beats resolving against a
        // target that is gone.
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
            // FUN_10423400. Width and height arrive by pointer at the caller's stack slots, which
            // 0x10423610 reads back after the call, so writing through them moves the viewport too.
            auto setResolution = dunia_pattern("81 EC 48 02 00 00 53 55 56 57 33 ED 55 8D 84 24 B4 00 00 00");
            if (setResolution.empty())
                return;

            // The only writers of those globals in the binary. Hooked past them so the engine's
            // own bookkeeping stays intact.
            auto presentParams = dunia_pattern("8B 56 1C 89 15 ? ? ? ? 8B 46 20 A3 ? ? ? ? 8A 4D 08 F6 D9");
            if (presentParams.empty())
                return;

            // FUN_1040F190. Pattern runs past the ten-byte body into the CC padding and the next
            // prologue on purpose: an identical setter body exists at 0x102795C4.
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

                const bool bWanted = nRequestedW > 0 && nRequestedH > 0;

                // Fullscreen, the boot path has already rounded this to a real adapter mode, which
                // matters because CreateDevice's failure path at 0x104252D9 is an infinite retry
                // loop. Windowed, it is replaced below with the measured client rect.
                nOutputW = *pWidth;
                nOutputH = *pHeight;

                if (!bWanted || nOutputW == 0 || nOutputH == 0)
                    return;

                // pConfig[9] is "use the desktop resolution". It makes GetScreenSize hand back the
                // window's client rect instead of the stored pair, which undoes the split.
                pConfig[9] = 0;

                nInternalW = static_cast<uint32_t>(nRequestedW);
                nInternalH = static_cast<uint32_t>(nRequestedH);

                *pWidth = nInternalW;
                *pHeight = nInternalH;
            });

            static auto BackBufferSizeHook = safetyhook::create_mid(presentParams.get_first(0x11), [](SafetyHookContext& regs)
            {
                if (nInternalW == 0 || nInternalH == 0 || nOutputW == 0 || nOutputH == 0 ||
                    !pBackBufferWidth || !pBackBufferHeight)
                    return;

                // EBP is the device config, ESI the renderer. The hook sits on the read of the
                // fullscreen byte for the presentation interval; DAT_11609D40 (Windowed) came from
                // the same byte a few instructions back and the three-branch aspect block read it
                // before that, with no write in between, so one reading answers for both.
                auto* pConfig = reinterpret_cast<uint8_t*>(regs.ebp);
                const bool bFullscreen = pConfig[8] != 0;

                if (!bFullscreen)
                {
                    // Measure the window rather than recompute it. Matters in borderless, where
                    // the window covers the display whatever the video options say.
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

                // Letterboxed, so the engine should compose for a screen shaped like the internal
                // frame. Only the fullscreen branch differs: it takes the config's stored aspect,
                // which describes the monitor.
                const auto fInternal = static_cast<float>(nInternalW) / static_cast<float>(nInternalH);
                const auto fOutput = static_cast<float>(nOutputW) / static_cast<float>(nOutputH);

                if (bFullscreen && std::fabs(fInternal - fOutput) > 0.0001f)
                    *reinterpret_cast<float*>(regs.esi + 0x24) = fInternal;
            });

            static auto WrapperSetterHook = safetyhook::create_mid(wrapperSetter.get_first(), [](SafetyHookContext& regs)
            {
                // __thiscall f(this in ECX, surface at [esp+4]), callee-cleaned. Rewriting the
                // stack slot is enough: the relocated MOV EAX,[ESP+4] reads it back.
                OnSetWrapperSurface(reinterpret_cast<IDirect3DSurface9**>(regs.esp + 0x04));
            });

            static auto FrameEndHook = safetyhook::create_mid(frameEnd.get_first(), [](SafetyHookContext& regs)
            {
                OnFrameEnd(regs.ecx);
            });

            static auto PreResetHook = safetyhook::create_mid(preReset.get_first(), [](SafetyHookContext& regs)
            {
                // Nothing this module owns may outlive the Reset four instructions from here.
                CResolver::ReleaseForReset(regs.ecx ? *reinterpret_cast<IDirect3DDevice9**>(regs.ecx + 0x38) : nullptr);
                bSubstituted = false;
            });

            static auto InternalResolutionCB = []()
            {
                nRequestedW = JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONX);
                nRequestedH = JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONY);
                nFilter = JackalFixSettings.GetInt(PREF_SCALINGFILTER);

                // One axis without the other is a typo.
                if (nRequestedW <= 0 || nRequestedH <= 0)
                {
                    nRequestedW = 0;
                    nRequestedH = 0;
                }
            };

            InternalResolutionCB();

            // Resolution is read at engine start and on every video-options apply.
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
