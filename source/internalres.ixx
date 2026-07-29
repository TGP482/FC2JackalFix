module;

#include <common.hxx>
#include <algorithm>
#include <utility>
#include <vector>
#include <cstdint>
#include <d3d9.h>

// An internal render resolution independent of the window, the display mode and the backbuffer,
// for supersampling above what the monitor can show or rendering below it and scaling back up.
//
// Dunia has exactly one number for "how big is the frame" and it is welded to the swapchain.
// FUN_10423400 (SetResolution) takes a width and a height, stores them at renderer+0x1C/+0x20,
// derives the aspect ratio into renderer+0x24, and copies the same pair straight into the global
// D3DPRESENT_PARAMETERS as BackBufferWidth/BackBufferHeight. Everything downstream follows the
// stored pair: the viewport is a fraction of it, and every scene render target is allocated from
// the viewport. The two quantities are written four instructions apart from the same registers,
// which is why they cannot diverge on their own - and why one choke point is enough to split them.
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
// is substituted. The engine never touches an IDirect3DSurface9* for the backbuffer directly - it
// goes through a 0x20-byte wrapper whose only setter is FUN_1040F190, three instructions long, and
// which caches no dimensions at all: vtable+0x10 is a live GetSize that calls the surface's
// GetDesc on every call. Install a different surface and every size query answers consistently,
// with no shadow fields to keep in sync. So the setter hands the engine an internal-res render
// target instead, and the frame function resolves that target onto the real backbuffer just before
// the engine's screenshot block and Present.
//
// Not letting D3D9 stretch a mismatched backbuffer at Present instead is deliberate. It does
// scale - Microsoft documents that much - but the documentation specifies no filter and in
// practice it is a point sample, every second pixel discarded going down. Supersampling through it
// is worse than not supersampling at all. This resolve picks its own filter and halves through
// scratch targets for ratios above 2:1, so a bilinear tap is always an exact 2x2 box average.
//
// All three display modes work. Exclusive fullscreen needs nothing special: the fullscreen branch
// of SetResolution already leaves renderer+0x90 clear, and the resolution arriving there has
// already been rounded to a real adapter mode by the boot path, so the backbuffer stays a mode the
// driver will accept - which matters, because CreateDevice's failure path at 0x104252D9 is an
// infinite retry loop rather than an error.
//
// The scale is uniform on both axes. An internal resolution shaped differently from the output -
// 4:3 on a 16:9 display, say - is fitted and centred with black bars rather than stretched, and the
// engine is told to compose for a screen the shape of the internal frame, so the geometry it draws
// is the geometry it would draw running natively at that resolution.
//
// Two consequences worth knowing. The HUD and menus are drawn by the engine into the same target
// as the scene, so they are resampled with it - sharper than native when supersampling, softer
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
    FILTER_CATMULLROM = 2,
};

static int32_t nRequestedW = 0;         // from the ini; 0 leaves the game entirely alone
static int32_t nRequestedH = 0;
static int32_t nFilter = FILTER_BILINEAR;

static uint32_t nInternalW = 0;         // what the engine believes the screen is
static uint32_t nInternalH = 0;
static uint32_t nOutputW = 0;           // what the swapchain actually presents
static uint32_t nOutputH = 0;

// The fullscreen byte is read once, at the present-parameter write, and answers two questions at
// the same time: which of SetResolution's three branches computed the aspect ratio, and whether the
// device is about to be windowed. That only works because DisplayMode settles the byte before the
// branch reads it - the old Borderless option cleared it after the branch instead, which made the
// two readings disagree and left a windowed device carrying a fullscreen aspect.

// The last surface the device confirmed as its own backbuffer. Only a fast path - the identity is
// re-established from the device whenever an unfamiliar surface turns up - so a device that is
// destroyed and recreated rather than reset needs no special handling.
static IDirect3DSurface9* pKnownBackBuffer = nullptr;
static bool bSubstituted = false;

// The two present-parameter globals, reached through the pattern's own operands. ASLR moves them,
// so a hardcoded address would be wrong the moment the module rebases.
static uint32_t* pBackBufferWidth = nullptr;
static uint32_t* pBackBufferHeight = nullptr;

// ---------------------------------------------------------------------------------------------
// The resolve pass

// A Catmull-Rom kernel for the final pass. It only earns its instruction count when scaling up:
// on the way down the halving chain has already box-filtered everything, and bilinear on the
// remainder is both cheaper and closer to correct.
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
    static inline IDirect3DPixelShader9* pCatmullRom = nullptr;
    // Kept separately from the shader object so a device that is reset - or destroyed and rebuilt -
    // costs a CreatePixelShader rather than another compile.
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

        // Nine dependent fetches and the weight math fit ps_3_0 comfortably and ps_2_0 only just,
        // so the higher profile is tried first rather than the other way round.
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

    // One textured quad from pSrc into a rect of pDst. The rect is the whole target for every
    // stage of the halving chain and, for the final pass, the aspect-preserving fit inside the
    // backbuffer - so bars are the only thing that ever separates the two.
    static bool Blit(IDirect3DDevice9* pDevice, IDirect3DSurface9* pDst, UINT nTargetW, UINT nTargetH,
                     int32_t nDstX, int32_t nDstY, UINT nDstW, UINT nDstH,
                     IDirect3DTexture9* pSrc, UINT nSrcW, UINT nSrcH, int32_t eFilter)
    {
        // Depth goes first every time. D3D9 rejects a render target smaller than the depth-stencil
        // currently bound to it, and while supersampling the engine's depth buffer is larger than
        // every target in this chain.
        pDevice->SetDepthStencilSurface(nullptr);

        if (FAILED(pDevice->SetRenderTarget(0, pDst)))
            return false;

        // The viewport is the whole target, not the fitted rect, so the clear below reaches the
        // bars. The quad is positioned instead.
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
        // The frame arriving here is already in whatever space the game finished in - the engine's
        // own generic surface copy never writes sRGB either - so converting on the way out would
        // shift every pixel.
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

        // Three states the engine may have left set that a state block only restores afterwards
        // rather than neutralising first, and that between them account for most of the ways a
        // fullscreen quad comes out wrong. Wrapping makes the 0-to-1 texcoord take the short way
        // round; the other two are vertex-stage states, so they bite the shader path as well.
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

    // Called at the top of the frame function, before the engine's screenshot block and Present.
    // Present is outside the scene bracket - DAT_11609D78 has no write anywhere inside 0x10424150
    // - which is what makes it legal to insert draw work here.
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
        // no better than none. The target is the fitted rect, not the backbuffer: with bars, the
        // reduction the final pass has to cover is the one that ends at the rect.
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

        // Both or neither. Slot 0 can never be unbound, so a failed GetRenderTarget would leave the
        // output-sized backbuffer bound - and binding the engine's larger depth buffer against it
        // would be rejected, costing the rest of the frame its depth.
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

        // Shaders survive Reset, but not a device that is destroyed and rebuilt. The compiled
        // bytecode is kept, so coming back costs a CreatePixelShader rather than another compile.
        SafeRelease(pCatmullRom);
    }

    // Process teardown. The device may already be gone, so nothing is asked of it.
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

// Renderer vtable +0x40, the end-of-frame function. `this` is in ECX at the entry and
// renderer+0x38 is the IDirect3DDevice9*.
static void OnFrameEnd(uintptr_t nRenderer)
{
    if (!bSubstituted || !nRenderer)
        return;

    CResolver::Resolve(*reinterpret_cast<IDirect3DDevice9**>(nRenderer + 0x38));
}

// FUN_1040F190, the wrapper's surface setter. It is generic - every render-target wrapper in the
// engine goes through it - so the backbuffer has to be recognised rather than assumed. The test is
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
        // for the internal resolution, so the frame will be wrong - but wrong and visible beats a
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
            // slots, which 0x10423610 reads back after the call to set the viewport - so a write
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

                const bool bWanted = nRequestedW > 0 && nRequestedH > 0;

                // What the engine was about to render at is what it should still present at. In
                // fullscreen that has already been rounded down to a real adapter mode by the boot
                // path, so it stays a mode CreateDevice will accept - which matters, because a mode
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
                    // client area cannot drift apart - including in borderless, where the window
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
                // the engine should compose for a screen shaped like the internal frame - which is
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

                // One axis without the other is a typo, not a request.
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
