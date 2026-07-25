module;

#include <common.hxx>
#include <vector>
#include <utility>
#include <cstdarg>
#include <cstdio>
#include <d3d9.h>
#include <d3d10.h>

// Xbox 360-style gamma correction applied as a fullscreen post-process.
// Runs at Present, matching the console's pipeline.

export module x360gamma;

import common;
import dunia;
import settings;



// Hook the renderer once per frame to discover the active D3D device, then
// hook the underlying D3D entry points through its vtable.
struct IShaderBlob : public IUnknown
{
    virtual void* STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};

using D3DXCompileShader_t = HRESULT(WINAPI*)(const char* pSrcData, UINT SrcDataLen, const void* pDefines,
    void* pInclude, const char* pFunctionName, const char* pProfile, DWORD Flags,
    IShaderBlob** ppShader, IShaderBlob** ppErrorMsgs, void** ppConstantTable);

using D3D10CompileShader_t = HRESULT(WINAPI*)(const char* pSrcData, SIZE_T SrcDataLen, const char* pFileName,
    const void* pDefines, void* pInclude, const char* pFunctionName, const char* pProfile, UINT Flags,
    IShaderBlob** ppShader, IShaderBlob** ppErrorMsgs);

template<typename T>
static void SafeRelease(T*& ptr)
{
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

// Render-thread diagnostics.
static void Log(const char* szFormat, ...)
{
    char szBuffer[512] = "FC2JackalFix: Xbox360Gamma - ";
    const auto nPrefix = strlen(szBuffer);

    va_list args;
    va_start(args, szFormat);
    std::vsnprintf(szBuffer + nPrefix, sizeof(szBuffer) - nPrefix - 2, szFormat, args);
    va_end(args);

    strcat_s(szBuffer, "\n");
    OutputDebugStringA(szBuffer);
}

// D3D9 and D3D10 renderers store their D3D interfaces at the same offsets.
static constexpr uintptr_t nRenderDeviceInterface = 0x38;
static constexpr uintptr_t nRenderDeviceSwapChain = 0x40;

// COM vtable indices from the interface declarations.
static constexpr size_t nDevice9Reset = 16;
static constexpr size_t nSwapChainPresent = 8;    // IUnknown 0-2, IDXGIObject 3-6, GetDevice 7
static constexpr size_t nSwapChainResizeBuffers = 13;

static bool bXbox360Gamma = false;
static bool bD3D9ResetHooked = false;
static bool bD3D10Hooked = false;
static void** ppRenderDevice = nullptr;

// Shared Xbox 360 gamma curve used by both shaders.
static constexpr const char* szGammaCurve = R"(
float X360GammaApprox(float x)
{
    float A = 0.541901f;
    float B = 1.13465f;
    float C = 13.53054f;
    float D = 6.56649f;
    float E = 0.311465f;

    x = max(0.0f, x);
    float f1 = A * x;
    float f2 = pow(x, B) * (1.0f - exp2(-C * x));
    float f3 = saturate(x * D + E);

    return lerp(f1, f2, f3);
}
)";

// ---------------------------------------------------------------------------------------------
// D3D9
//
// D3D9 uses a simple ps_2_0 fullscreen pass with fixed-function vertex processing.
static constexpr const char* szPixelShader9 = R"(
sampler2D InputTex : register(s0);

float4 ConsoleGammaPS(in float2 uv : TEXCOORD0) : COLOR0
{
    float3 color = tex2D(InputTex, uv).rgb;

    return float4(X360GammaApprox(color.r),
                  X360GammaApprox(color.g),
                  X360GammaApprox(color.b), 1.0f);
}
)";

class CGammaD3D9
{
    struct ScreenVertex { float x, y, z, rhw, u, v; };
    static constexpr DWORD nScreenFVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

    static inline std::vector<uint8_t> vBytecode;
    static inline bool bCompileFailed = false;

    static inline IDirect3DPixelShader9* pPixelShader = nullptr;
    static inline IDirect3DTexture9* pCopyTex = nullptr;
    static inline IDirect3DSurface9* pCopySurf = nullptr;
    static inline IDirect3DStateBlock9* pStateBlock = nullptr;

    static inline UINT nWidth = 0;
    static inline UINT nHeight = 0;
    static inline D3DFORMAT eFormat = D3DFMT_UNKNOWN;

    // Dunia imports d3dx9_38. Other names support installs with different D3DX versions.
    static bool CompileShader()
    {
        if (!vBytecode.empty())
            return true;
        if (bCompileFailed)
            return false;

        bCompileFailed = true;

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
        {
            Log("no loaded d3dx9 module exports D3DXCompileShader");
            return false;
        }

        const std::string strSource = std::string(szGammaCurve) + szPixelShader9;

        IShaderBlob* pShader = nullptr;
        IShaderBlob* pErrors = nullptr;
        const auto hr = pCompile(strSource.c_str(), static_cast<UINT>(strSource.size()), nullptr, nullptr,
                                 "ConsoleGammaPS", "ps_2_0", 0, &pShader, &pErrors, nullptr);

        if (FAILED(hr) || !pShader)
        {
            Log("D3DXCompileShader failed, hr %#lx", static_cast<unsigned long>(hr));
            if (pErrors)
            {
                OutputDebugStringA(static_cast<const char*>(pErrors->GetBufferPointer()));
                pErrors->Release();
            }
            SafeRelease(pShader);
            return false;
        }

        const auto* pStart = static_cast<const uint8_t*>(pShader->GetBufferPointer());
        vBytecode.assign(pStart, pStart + pShader->GetBufferSize());

        SafeRelease(pErrors);
        SafeRelease(pShader);

        Log("ps_2_0 shader compiled, %zu bytes", vBytecode.size());

        bCompileFailed = false;
        return true;
    }

    static bool EnsureResources(IDirect3DDevice9* pDevice, const D3DSURFACE_DESC& desc)
    {
        if (pPixelShader && pCopyTex && pCopySurf && pStateBlock &&
            desc.Width == nWidth && desc.Height == nHeight && desc.Format == eFormat)
            return true;

        Release();

        if (!CompileShader())
            return false;

        nWidth = desc.Width;
        nHeight = desc.Height;
        eFormat = desc.Format;

        if (FAILED(pDevice->CreatePixelShader(reinterpret_cast<const DWORD*>(vBytecode.data()), &pPixelShader)))
        {
            Release();
            return false;
        }

        // The copy texture is never multisampled. StretchRect resolves MSAA backbuffers,
        // so the filter must remain D3DTEXF_NONE.
        if (FAILED(pDevice->CreateTexture(nWidth, nHeight, 1, D3DUSAGE_RENDERTARGET, eFormat,
                                          D3DPOOL_DEFAULT, &pCopyTex, nullptr)))
        {
            Release();
            return false;
        }

        if (FAILED(pCopyTex->GetSurfaceLevel(0, &pCopySurf)))
        {
            Release();
            return false;
        }

        // Created once per device. Capture() refreshes the live state values each frame.
        if (FAILED(pDevice->CreateStateBlock(D3DSBT_ALL, &pStateBlock)))
        {
            Release();
            return false;
        }

        return true;
    }

public:
    // Called before Reset and during shutdown. D3DPOOL_DEFAULT resources must be
    // released before Reset succeeds.
    static void Release()
    {
        SafeRelease(pStateBlock);
        SafeRelease(pCopySurf);
        SafeRelease(pCopyTex);
        SafeRelease(pPixelShader);
        nWidth = 0;
        nHeight = 0;
        eFormat = D3DFMT_UNKNOWN;
    }

    static void Render(IDirect3DDevice9* pDevice)
    {
        if (!bXbox360Gamma || !pDevice)
            return;

        IDirect3DSurface9* pBackBuffer = nullptr;
        if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) || !pBackBuffer)
            return;

        D3DSURFACE_DESC desc{};
        pBackBuffer->GetDesc(&desc);

        if (!EnsureResources(pDevice, desc))
        {
            static bool bWarned = false;
            if (!std::exchange(bWarned, true))
                Log("D3D9 resources unavailable, %ux%u fmt %d", desc.Width, desc.Height, desc.Format);

            SafeRelease(pBackBuffer);
            return;
        }

        if (const auto hr = pDevice->StretchRect(pBackBuffer, nullptr, pCopySurf, nullptr, D3DTEXF_NONE); FAILED(hr))
        {
            static bool bWarned = false;
            if (!std::exchange(bWarned, true))
                Log("StretchRect off the backbuffer failed, hr %#lx (samples %d)", static_cast<unsigned long>(hr), desc.MultiSampleType);

            SafeRelease(pBackBuffer);
            return;
        }

        static bool bAnnounced = false;
        if (!std::exchange(bAnnounced, true))
            Log("D3D9 pass running, %ux%u fmt %d", desc.Width, desc.Height, desc.Format);

        pStateBlock->Capture();

        // A state block does not cover render targets, so those are saved by hand.
        IDirect3DSurface9* pOldTarget = nullptr;
        IDirect3DSurface9* pOldDepth = nullptr;
        pDevice->GetRenderTarget(0, &pOldTarget);
        pDevice->GetDepthStencilSurface(&pOldDepth);

        pDevice->SetRenderTarget(0, pBackBuffer);
        pDevice->SetDepthStencilSurface(nullptr);

        pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
        pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
        // Keep clipping enabled. The fullscreen quad does not require clipping to be disabled.
        pDevice->SetRenderState(D3DRS_CLIPPING, TRUE);
        pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);
        pDevice->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
        // Disabled intentionally. The curve operates on the game's output values directly.
        pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        for (DWORD i = 1; i < 8; ++i)
            pDevice->SetTexture(i, nullptr);

        pDevice->SetTexture(0, pCopyTex);
        // Point sampling, because this is a 1:1 blit and anything else would soften the frame.
        pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

        pDevice->SetVertexShader(nullptr);
        pDevice->SetPixelShader(pPixelShader);
        pDevice->SetFVF(nScreenFVF);
        pDevice->SetIndices(nullptr);

        const auto fWidth = static_cast<float>(desc.Width);
        const auto fHeight = static_cast<float>(desc.Height);

        // Apply D3D9's half-pixel texel-centre offset.
        const ScreenVertex verts[4] =
        {
            { -0.5f,           -0.5f,            0.0f, 1.0f, 0.0f, 0.0f },
            { fWidth - 0.5f,   -0.5f,            0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f,           fHeight - 0.5f,   0.0f, 1.0f, 0.0f, 1.0f },
            { fWidth - 0.5f,   fHeight - 0.5f,   0.0f, 1.0f, 1.0f, 1.0f },
        };

        // Present is outside the engine scene. Only close the scene if we opened it.
        const bool bOpenedScene = SUCCEEDED(pDevice->BeginScene());
        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ScreenVertex));
        if (bOpenedScene)
            pDevice->EndScene();

        pDevice->SetRenderTarget(0, pOldTarget);
        pDevice->SetDepthStencilSurface(pOldDepth);
        pStateBlock->Apply();

        SafeRelease(pOldDepth);
        SafeRelease(pOldTarget);
        SafeRelease(pBackBuffer);
    }
};

// ---------------------------------------------------------------------------------------------
// D3D10
//
// Fullscreen triangle generated from SV_VertexID. No vertex buffer or input layout needed.
static constexpr const char* szShader10 = R"(
struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 Texcoord : TEXCOORD0;
};

VS_OUTPUT FullscreenVS(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    output.Texcoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.Texcoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

Texture2D InputTexture : register(t0);
SamplerState PointClamp : register(s0);

float4 ConsoleGammaPS(VS_OUTPUT input) : SV_TARGET
{
    float3 color = InputTexture.Sample(PointClamp, input.Texcoord).rgb;

    return float4(X360GammaApprox(color.r),
                  X360GammaApprox(color.g),
                  X360GammaApprox(color.b), 1.0f);
}
)";

class CGammaD3D10
{
    static inline bool bSetupFailed = false;

    static inline ID3D10Device* pDevice = nullptr;
    static inline ID3D10VertexShader* pVertexShader = nullptr;
    static inline ID3D10PixelShader* pPixelShader = nullptr;
    static inline ID3D10SamplerState* pSampler = nullptr;
    static inline ID3D10BlendState* pBlendState = nullptr;
    static inline ID3D10DepthStencilState* pDepthState = nullptr;
    static inline ID3D10RasterizerState* pRasterState = nullptr;

    static inline ID3D10Texture2D* pCopyTex = nullptr;
    static inline ID3D10ShaderResourceView* pCopySRV = nullptr;
    static inline UINT nWidth = 0;
    static inline UINT nHeight = 0;

    struct PipelineState
    {
        ID3D10RenderTargetView* pTarget = nullptr;
        ID3D10DepthStencilView* pDepth = nullptr;
        ID3D10BlendState* pBlend = nullptr;
        FLOAT fBlendFactor[4] = {};
        UINT nSampleMask = 0;
        ID3D10DepthStencilState* pDepthState = nullptr;
        UINT nStencilRef = 0;
        ID3D10RasterizerState* pRaster = nullptr;
        ID3D10VertexShader* pVS = nullptr;
        ID3D10GeometryShader* pGS = nullptr;
        ID3D10PixelShader* pPS = nullptr;
        ID3D10InputLayout* pLayout = nullptr;
        D3D10_PRIMITIVE_TOPOLOGY eTopology = D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ID3D10ShaderResourceView* pSRV = nullptr;
        ID3D10SamplerState* pSamplerState = nullptr;
        UINT nViewports = 0;
        D3D10_VIEWPORT Viewports[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    };

    static bool CompileOne(D3D10CompileShader_t pCompile, const std::string& strSource,
                           const char* szEntry, const char* szProfile, IShaderBlob** ppOut)
    {
        IShaderBlob* pErrors = nullptr;
        const auto hr = pCompile(strSource.c_str(), strSource.size(), "Xbox360Gamma", nullptr, nullptr,
                                 szEntry, szProfile, 0, ppOut, &pErrors);

        if (FAILED(hr) || !*ppOut)
        {
            Log("D3D10CompileShader failed for %s (%s), hr %#lx", szEntry, szProfile, static_cast<unsigned long>(hr));
            if (pErrors)
            {
                OutputDebugStringA(static_cast<const char*>(pErrors->GetBufferPointer()));
                pErrors->Release();
            }
            return false;
        }

        SafeRelease(pErrors);
        return true;
    }

    static bool Setup(ID3D10Device* pFromSwapChain)
    {
        if (pDevice)
            return true;
        if (bSetupFailed)
            return false;

        bSetupFailed = true;

        // Resolve from the module already loaded by the engine.
        auto hD3D10 = GetModuleHandleW(L"d3d10.dll");
        auto pCompile = hD3D10 ? reinterpret_cast<D3D10CompileShader_t>(GetProcAddress(hD3D10, "D3D10CompileShader")) : nullptr;
        if (!pCompile)
        {
            Log("d3d10.dll does not export D3D10CompileShader (module %p)", static_cast<void*>(hD3D10));
            return false;
        }

        const std::string strSource = std::string(szGammaCurve) + szShader10;

        IShaderBlob* pVSBlob = nullptr;
        IShaderBlob* pPSBlob = nullptr;

        if (!CompileOne(pCompile, strSource, "FullscreenVS", "vs_4_0", &pVSBlob) ||
            !CompileOne(pCompile, strSource, "ConsoleGammaPS", "ps_4_0", &pPSBlob))
        {
            SafeRelease(pVSBlob);
            SafeRelease(pPSBlob);
            return false;
        }

        auto bOk = SUCCEEDED(pFromSwapChain->CreateVertexShader(pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), &pVertexShader)) &&
                   SUCCEEDED(pFromSwapChain->CreatePixelShader(pPSBlob->GetBufferPointer(), pPSBlob->GetBufferSize(), &pPixelShader));

        SafeRelease(pVSBlob);
        SafeRelease(pPSBlob);

        if (!bOk)
        {
            Release();
            return false;
        }

        D3D10_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D10_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.ComparisonFunc = D3D10_COMPARISON_NEVER;
        samplerDesc.MaxLOD = D3D10_FLOAT32_MAX;

        // Blending is off, but D3D10 still validates the blend factors, so they get real values.
        D3D10_BLEND_DESC blendDesc{};
        blendDesc.SrcBlend = D3D10_BLEND_ONE;
        blendDesc.DestBlend = D3D10_BLEND_ZERO;
        blendDesc.BlendOp = D3D10_BLEND_OP_ADD;
        blendDesc.SrcBlendAlpha = D3D10_BLEND_ONE;
        blendDesc.DestBlendAlpha = D3D10_BLEND_ZERO;
        blendDesc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
        for (auto& nMask : blendDesc.RenderTargetWriteMask)
            nMask = D3D10_COLOR_WRITE_ENABLE_ALL;

        D3D10_DEPTH_STENCIL_DESC depthDesc{};
        depthDesc.DepthEnable = FALSE;
        depthDesc.StencilEnable = FALSE;

        D3D10_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D10_FILL_SOLID;
        rasterDesc.CullMode = D3D10_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        rasterDesc.ScissorEnable = FALSE;

        bOk = SUCCEEDED(pFromSwapChain->CreateSamplerState(&samplerDesc, &pSampler)) &&
              SUCCEEDED(pFromSwapChain->CreateBlendState(&blendDesc, &pBlendState)) &&
              SUCCEEDED(pFromSwapChain->CreateDepthStencilState(&depthDesc, &pDepthState)) &&
              SUCCEEDED(pFromSwapChain->CreateRasterizerState(&rasterDesc, &pRasterState));

        if (!bOk)
        {
            Release();
            return false;
        }

        pDevice = pFromSwapChain;
        pDevice->AddRef();

        Log("D3D10 pass ready");

        bSetupFailed = false;
        return true;
    }

    static bool EnsureCopyTexture(const D3D10_TEXTURE2D_DESC& backBufferDesc)
    {
        if (pCopySRV && backBufferDesc.Width == nWidth && backBufferDesc.Height == nHeight)
            return true;

        SafeRelease(pCopySRV);
        SafeRelease(pCopyTex);

        nWidth = backBufferDesc.Width;
        nHeight = backBufferDesc.Height;

        D3D10_TEXTURE2D_DESC copyDesc = backBufferDesc;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        // Always single sampled. MSAA backbuffers are resolved before copying.
        copyDesc.SampleDesc.Count = 1;
        copyDesc.SampleDesc.Quality = 0;
        copyDesc.Usage = D3D10_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;

        if (FAILED(pDevice->CreateTexture2D(&copyDesc, nullptr, &pCopyTex)))
        {
            nWidth = nHeight = 0;
            return false;
        }

        D3D10_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = copyDesc.Format;
        srvDesc.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        if (FAILED(pDevice->CreateShaderResourceView(pCopyTex, &srvDesc, &pCopySRV)))
        {
            SafeRelease(pCopyTex);
            nWidth = nHeight = 0;
            return false;
        }

        return true;
    }

    static void SaveState(PipelineState& state)
    {
        pDevice->OMGetRenderTargets(1, &state.pTarget, &state.pDepth);
        pDevice->OMGetBlendState(&state.pBlend, state.fBlendFactor, &state.nSampleMask);
        pDevice->OMGetDepthStencilState(&state.pDepthState, &state.nStencilRef);
        pDevice->RSGetState(&state.pRaster);
        pDevice->VSGetShader(&state.pVS);
        pDevice->GSGetShader(&state.pGS);
        pDevice->PSGetShader(&state.pPS);
        pDevice->IAGetInputLayout(&state.pLayout);
        pDevice->IAGetPrimitiveTopology(&state.eTopology);
        pDevice->PSGetShaderResources(0, 1, &state.pSRV);
        pDevice->PSGetSamplers(0, 1, &state.pSamplerState);
        state.nViewports = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        pDevice->RSGetViewports(&state.nViewports, state.Viewports);
    }

    static void RestoreState(PipelineState& state)
    {
        pDevice->OMSetRenderTargets(1, &state.pTarget, state.pDepth);
        pDevice->OMSetBlendState(state.pBlend, state.fBlendFactor, state.nSampleMask);
        pDevice->OMSetDepthStencilState(state.pDepthState, state.nStencilRef);
        pDevice->RSSetState(state.pRaster);
        pDevice->VSSetShader(state.pVS);
        pDevice->GSSetShader(state.pGS);
        pDevice->PSSetShader(state.pPS);
        pDevice->IASetInputLayout(state.pLayout);
        pDevice->IASetPrimitiveTopology(state.eTopology);
        pDevice->PSSetShaderResources(0, 1, &state.pSRV);
        pDevice->PSSetSamplers(0, 1, &state.pSamplerState);
        pDevice->RSSetViewports(state.nViewports, state.Viewports);

        // OMGet/RSGet/VSGet add references that must be released after restoring state.
        SafeRelease(state.pTarget);
        SafeRelease(state.pDepth);
        SafeRelease(state.pBlend);
        SafeRelease(state.pDepthState);
        SafeRelease(state.pRaster);
        SafeRelease(state.pVS);
        SafeRelease(state.pGS);
        SafeRelease(state.pPS);
        SafeRelease(state.pLayout);
        SafeRelease(state.pSRV);
        SafeRelease(state.pSamplerState);
    }

public:
    static void OnResize()
    {
        SafeRelease(pCopySRV);
        SafeRelease(pCopyTex);
        nWidth = 0;
        nHeight = 0;
    }

    static void Release()
    {
        OnResize();
        SafeRelease(pRasterState);
        SafeRelease(pDepthState);
        SafeRelease(pBlendState);
        SafeRelease(pSampler);
        SafeRelease(pPixelShader);
        SafeRelease(pVertexShader);
        SafeRelease(pDevice);
    }

    static void Render(IDXGISwapChain* pSwapChain)
    {
        if (!bXbox360Gamma || !pSwapChain)
            return;

        if (!pDevice)
        {
            ID3D10Device* pFromSwapChain = nullptr;
            if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device), reinterpret_cast<void**>(&pFromSwapChain))) || !pFromSwapChain)
                return;

            const auto bReady = Setup(pFromSwapChain);
            pFromSwapChain->Release();
            if (!bReady)
                return;
        }

        ID3D10Texture2D* pBackBuffer = nullptr;
        if (FAILED(pSwapChain->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&pBackBuffer))) || !pBackBuffer)
            return;

        D3D10_TEXTURE2D_DESC backBufferDesc{};
        pBackBuffer->GetDesc(&backBufferDesc);

        if (!EnsureCopyTexture(backBufferDesc))
        {
            SafeRelease(pBackBuffer);
            return;
        }

        // Recreated each frame. ResizeBuffers replaces the backbuffer resource.
        ID3D10RenderTargetView* pTargetView = nullptr;
        if (FAILED(pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pTargetView)))
        {
            SafeRelease(pBackBuffer);
            return;
        }

        PipelineState state{};
        SaveState(state);

        if (backBufferDesc.SampleDesc.Count > 1)
            pDevice->ResolveSubresource(pCopyTex, 0, pBackBuffer, 0, backBufferDesc.Format);
        else
            pDevice->CopyResource(pCopyTex, pBackBuffer);

        SafeRelease(pBackBuffer);

        const D3D10_VIEWPORT viewport = { 0, 0, nWidth, nHeight, 0.0f, 1.0f };
        pDevice->RSSetViewports(1, &viewport);
        pDevice->RSSetState(pRasterState);

        pDevice->OMSetRenderTargets(1, &pTargetView, nullptr);
        pDevice->OMSetBlendState(pBlendState, nullptr, 0xFFFFFFFF);
        pDevice->OMSetDepthStencilState(pDepthState, 0);

        pDevice->IASetInputLayout(nullptr);
        pDevice->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        pDevice->VSSetShader(pVertexShader);
        pDevice->GSSetShader(nullptr);
        pDevice->PSSetShader(pPixelShader);
        pDevice->PSSetShaderResources(0, 1, &pCopySRV);
        pDevice->PSSetSamplers(0, 1, &pSampler);

        pDevice->Draw(3, 0);

        // Unbound before the render target is, so the copy is never both source and destination.
        ID3D10ShaderResourceView* pNullSRV = nullptr;
        pDevice->PSSetShaderResources(0, 1, &pNullSRV);

        SafeRelease(pTargetView);

        RestoreState(state);
    }
};

// ---------------------------------------------------------------------------------------------
// Hooks

static SafetyHookInline shDevice9Reset{};
static SafetyHookInline shSwapChainPresent{};
static SafetyHookInline shSwapChainResizeBuffers{};

static HRESULT __stdcall Device9Reset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pParameters)
{
    // D3DPOOL_DEFAULT resources have to be released before the reset, or the reset fails.
    CGammaD3D9::Release();
    return shDevice9Reset.unsafe_stdcall<HRESULT>(pDevice, pParameters);
}

static HRESULT __stdcall SwapChainPresent(IDXGISwapChain* pSwapChain, UINT nSyncInterval, UINT nFlags)
{
    CGammaD3D10::Render(pSwapChain);
    return shSwapChainPresent.unsafe_stdcall<HRESULT>(pSwapChain, nSyncInterval, nFlags);
}

static HRESULT __stdcall SwapChainResizeBuffers(IDXGISwapChain* pSwapChain, UINT nBufferCount, UINT nWidth,
                                                UINT nHeight, DXGI_FORMAT eFormat, UINT nSwapChainFlags)
{
    CGammaD3D10::OnResize();
    return shSwapChainResizeBuffers.unsafe_stdcall<HRESULT>(pSwapChain, nBufferCount, nWidth, nHeight, eFormat, nSwapChainFlags);
}

static void* GetVTableEntry(void* pObject, size_t nIndex)
{
    return (*reinterpret_cast<void***>(pObject))[nIndex];
}

// D3D9 present hook provides the device directly. Reset is hooked to release
// D3DPOOL_DEFAULT resources before device resets.
static void OnPresentD3D9(void* pRenderDevice)
{
    auto* pDevice = *reinterpret_cast<IDirect3DDevice9**>(reinterpret_cast<uintptr_t>(pRenderDevice) + nRenderDeviceInterface);
    if (!pDevice)
        return;

    if (!std::exchange(bD3D9ResetHooked, true))
    {
        shDevice9Reset = safetyhook::create_inline(GetVTableEntry(pDevice, nDevice9Reset), Device9Reset);
        Log("D3D9 present call site reached. Device %p, Reset hooked=%d",
            static_cast<void*>(pDevice), shDevice9Reset.enabled());
    }

    CGammaD3D9::Render(pDevice);
}

// DX10 initialization waits for the swap chain, which is created after the device.
// Returns true once initialization has been attempted.
static bool InstallD3D10Hooks(void* pRenderDevice)
{
    auto* pInterface = *reinterpret_cast<IUnknown**>(reinterpret_cast<uintptr_t>(pRenderDevice) + nRenderDeviceInterface);
    if (!pInterface)
        return false;

    // Use QueryInterface instead of vtable checks so DLL rebases do not matter.
    // D3D9 devices return E_NOINTERFACE and use the D3D9 path.
    ID3D10Device* pDevice10 = nullptr;
    if (FAILED(pInterface->QueryInterface(__uuidof(ID3D10Device), reinterpret_cast<void**>(&pDevice10))) || !pDevice10)
    {
        Log("render device %p at +0x%X is not an ID3D10Device - DX9 path", pRenderDevice,
            static_cast<unsigned>(nRenderDeviceInterface));
        return true;
    }

    pDevice10->Release();

    auto* pSwapChain = *reinterpret_cast<IDXGISwapChain**>(reinterpret_cast<uintptr_t>(pRenderDevice) + nRenderDeviceSwapChain);
    if (!pSwapChain)
    {
        static bool bWaited = false;
        if (!std::exchange(bWaited, true))
            Log("D3D10 renderer, waiting for the swap chain at +0x%X", static_cast<unsigned>(nRenderDeviceSwapChain));

        return false;
    }

    shSwapChainPresent = safetyhook::create_inline(GetVTableEntry(pSwapChain, nSwapChainPresent), SwapChainPresent);
    shSwapChainResizeBuffers = safetyhook::create_inline(GetVTableEntry(pSwapChain, nSwapChainResizeBuffers), SwapChainResizeBuffers);

    Log("D3D10 renderer. Swap chain %p, Present hooked=%d, ResizeBuffers hooked=%d",
        static_cast<void*>(pSwapChain), shSwapChainPresent.enabled(), shSwapChainResizeBuffers.enabled());

    return true;
}

class Xbox360Gamma
{
public:
    Xbox360Gamma()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            static auto Xbox360GammaCB = []()
            {
                bXbox360Gamma = JackalFixSettings.GetInt(PREF_X360GAMMA) != 0;
                Log("setting is %s", bXbox360Gamma ? "on" : "off");
            };

            Xbox360GammaCB();

            JackalFix::onIniFileChange() += []()
            {
                Xbox360GammaCB();
            };

            // Hook the engine's Present call site to get the D3D9 device directly.
            auto d3d9Present = dunia_pattern("8B 46 38 8B 08 83 C4 08 53 52 8B 54 24 24 52 8B 54 24 2C 52 50 8B 41 44 FF D0");
            if (d3d9Present.empty())
                Log("D3D9 present pattern did not match - DX9 renderer unsupported on this build");
            else
            {
                Log("D3D9 present hook at %p", d3d9Present.get_first());

                static auto D3D9PresentHook = safetyhook::create_mid(d3d9Present.get_first(), [](SafetyHookContext& regs)
                {
                    if (bXbox360Gamma)
                        OnPresentD3D9(reinterpret_cast<void*>(regs.esi));
                });
            }

            // Reach the render device global used to access the DX10 swap chain.
            auto renderDevice = dunia_pattern("8B 0D ? ? ? ? 8B 01 8B 90 EC 00 00 00 FF D2");
            if (renderDevice.empty())
            {
                Log("render device pattern did not match - DX10 renderer unsupported on this build");
                return;
            }

            ppRenderDevice = *renderDevice.get_first<void**>(2);
            Log("render device global at %p", static_cast<void*>(ppRenderDevice));

            static auto FrameHook = safetyhook::create_mid(renderDevice.get_first(), [](SafetyHookContext&)
            {
                if (bD3D10Hooked || !bXbox360Gamma)
                    return;

                if (auto* pRenderDevice = *ppRenderDevice)
                    bD3D10Hooked = InstallD3D10Hooks(pRenderDevice);
            });
        };

        JackalFix::onShutdownEvent() += []()
        {
            CGammaD3D9::Release();
            CGammaD3D10::Release();
        };
    }
} Xbox360Gamma;
