module;

#include <common.hxx>
#include <array>
#include <format>
#include <string>
#include <cstring>
#include <vector>
#include <d3d9.h>
#include <d3d10.h>
#include <d3dcompiler.h>

export module volumetricdevice;

import logging;
import volumetricshaders;

// Draws the volumetric passes on whichever renderer is running. The scene colour and the resolved
// view depth come from the engine's render target objects; everything the passes change is saved
// first and put back after, so the renderers' own state shadows stay true.

export enum class VolTexture : uint32_t
{
    None,
    FogA,
    FogB,
    Count,
};

export enum class VolShader : uint32_t
{
    FogMarch,
    FogBlur,
    FogComposite,
    Count,
};

export enum class VolBlend : uint32_t
{
    Replace,
    Over,
};

// eOutput None draws into the scene. s0 is always the resolved depth.
export struct VolDraw
{
    VolShader eShader;
    VolTexture eInput;
    VolTexture eOutput;
    VolBlend eBlend;
};

export struct VolConstants
{
    float c[nVolRegisters][4]{};
};

export struct VolSize
{
    uint32_t w = 0;
    uint32_t h = 0;
};

export class VolumetricDevice
{
public:
    virtual ~VolumetricDevice() = default;

    // The scene colour pass's engine render target and the engine texture behind DepthVPSampler.
    virtual bool Begin(uintptr_t nSceneTarget, uintptr_t nDepthTexture) = 0;
    virtual void End() = 0;
    virtual VolSize SceneSize() const = 0;
    // RGB8 packed depth, which wants the provider's unpack weights; float depth is metres.
    virtual bool DepthPacked() const = 0;
    virtual bool EnsureTarget(VolTexture eTexture, VolSize size) = 0;
    virtual void Draw(const VolDraw& draw, const VolConstants& constants) = 0;
    virtual bool ReadDepthCentre(float rgb[3]) = 0;
    // Where the depth was found, or why Begin failed.
    virtual const std::string& Status() const = 0;
    virtual void Release() = 0;
};

template<typename T>
static void SafeRelease(T*& ptr)
{
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

export bool Readable(uintptr_t nAddress, size_t nSize)
{
    return nAddress >= 0x10000 && !IsBadReadPtr(reinterpret_cast<const void*>(nAddress), nSize);
}

export template<typename T>
T ReadPointer(uintptr_t nAddress)
{
    return Readable(nAddress, sizeof(T)) ? *reinterpret_cast<T*>(nAddress) : T{};
}

static HMODULE VTableModule(uintptr_t nObject)
{
    const auto nVTable = ReadPointer<uintptr_t>(nObject);
    HMODULE hModule = nullptr;
    if (Readable(nVTable, 3 * sizeof(void*)))
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(nVTable), &hModule);
    return hModule;
}

// Engine textures hold their D3D object in themselves or one wrapper down; any pointer whose vtable
// lives in the D3D runtime is a COM object, so it is safe to query.
static constexpr uintptr_t EngineWrappers[] = { 0x00, 0x08, 0x0C };
static constexpr uintptr_t nEngineScanBytes = 0x40;

static IUnknown* FindD3DObject(uintptr_t nEngineObject, HMODULE hRuntime, REFIID iid, std::string& where)
{
    for (const auto nWrapper : EngineWrappers)
    {
        const auto nBase = nWrapper ? ReadPointer<uintptr_t>(nEngineObject + nWrapper) : nEngineObject;
        for (uintptr_t nOffset = 0; nBase && nOffset < nEngineScanBytes; nOffset += sizeof(void*))
        {
            const auto nCandidate = ReadPointer<uintptr_t>(nBase + nOffset);
            if (!nCandidate || VTableModule(nCandidate) != hRuntime)
                continue;

            IUnknown* pFound = nullptr;
            if (FAILED(reinterpret_cast<IUnknown*>(nCandidate)->QueryInterface(iid, reinterpret_cast<void**>(&pFound))))
                continue;

            where = std::format("wrapper +0x{:X} offset +0x{:X}", nWrapper, nOffset);
            return pFound;
        }
    }
    return nullptr;
}

static constexpr std::array<const char*, static_cast<size_t>(VolShader::Count)> ShaderDefines
{
    "FOG_MARCH",
    "FOG_BLUR",
    "FOG_COMPOSITE",
};

// System d3dcompiler_47, so nothing is linked against a redistributable.
static pD3DCompile Compiler()
{
    static auto pCompile = []() -> pD3DCompile
    {
        auto hModule = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!hModule)
            hModule = LoadLibraryW(L"d3dcompiler_43.dll");
        return hModule ? reinterpret_cast<pD3DCompile>(GetProcAddress(hModule, "D3DCompile")) : nullptr;
    }();
    return pCompile;
}

static bool CompileShader(const char* szEntry, const char* szProfile, std::initializer_list<const char*> defines,
                          std::vector<uint8_t>& vCode)
{
    auto pCompile = Compiler();
    if (!pCompile)
    {
        LogWarn("volumetrics: no d3dcompiler_47.dll");
        return false;
    }

    std::vector<D3D_SHADER_MACRO> vMacros;
    for (auto* szName : defines)
        vMacros.push_back({ szName, "1" });
    vMacros.push_back({ nullptr, nullptr });

    ID3DBlob* pCode = nullptr;
    ID3DBlob* pErrors = nullptr;
    const UINT nFlags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const auto hr = pCompile(szVolumetricShaders, strlen(szVolumetricShaders), "volumetrics", vMacros.data(), nullptr,
                             szEntry, szProfile, nFlags, 0, &pCode, &pErrors);

    if (SUCCEEDED(hr) && pCode)
    {
        const auto* pStart = static_cast<const uint8_t*>(pCode->GetBufferPointer());
        vCode.assign(pStart, pStart + pCode->GetBufferSize());
    }
    else
    {
        const char* szError = pErrors ? static_cast<const char*>(pErrors->GetBufferPointer()) : "unknown error";
        LogWarn("volumetrics: {} {} failed: {}", szProfile, *defines.begin(), szError);
    }

    SafeRelease(pErrors);
    SafeRelease(pCode);
    return !vCode.empty();
}

static float HalfToFloat(uint16_t nHalf)
{
    const uint32_t nSign = (nHalf & 0x8000u) << 16;
    const uint32_t nExponent = (nHalf >> 10) & 0x1Fu;
    const uint32_t nMantissa = nHalf & 0x3FFu;
    uint32_t nBits = nSign;

    if (nExponent == 0x1F)
        nBits |= 0x7F800000u | (nMantissa << 13);
    else if (nExponent != 0)
        nBits |= ((nExponent + 112) << 23) | (nMantissa << 13);

    float fValue = 0.0f;
    std::memcpy(&fValue, &nBits, sizeof(fValue));
    return fValue;
}

static VolSize LowResSize(VolTexture eTexture, VolSize scene)
{
    switch (eTexture)
    {
    case VolTexture::FogA:
    case VolTexture::FogB:
        return { (scene.w + 1) / 2, (scene.h + 1) / 2 };
    default:
        return scene;
    }
}

// Direct3D 9

namespace dx9
{

// Engine render target object, as FUN_104236A0 binds it.
static constexpr uintptr_t nTargetSurface = 0x1C;

static constexpr DWORD nMaxTargets = 4;

struct Target
{
    IDirect3DTexture9* pTex = nullptr;
    IDirect3DSurface9* pSurf = nullptr;
    VolSize size{};

    void Release()
    {
        SafeRelease(pSurf);
        SafeRelease(pTex);
        size = {};
    }

    bool Ensure(IDirect3DDevice9* pDevice, VolSize wanted)
    {
        if (pSurf && size.w == wanted.w && size.h == wanted.h)
            return true;

        Release();
        if (FAILED(pDevice->CreateTexture(wanted.w, wanted.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F,
                                          D3DPOOL_DEFAULT, &pTex, nullptr)))
            return false;

        if (FAILED(pTex->GetSurfaceLevel(0, &pSurf)))
        {
            Release();
            return false;
        }

        size = wanted;
        return true;
    }
};

static bool IsPackedDepth(D3DFORMAT eFormat)
{
    return eFormat == D3DFMT_A8R8G8B8 || eFormat == D3DFMT_X8R8G8B8;
}

static bool IsFloatDepth(D3DFORMAT eFormat)
{
    return eFormat == D3DFMT_R32F || eFormat == D3DFMT_R16F || eFormat == D3DFMT_G16R16F ||
           eFormat == D3DFMT_G32R32F || eFormat == D3DFMT_A16B16G16R16F || eFormat == D3DFMT_A32B32G32R32F;
}

class Device final : public VolumetricDevice
{
    IDirect3DDevice9* pDevice = nullptr;
    IDirect3DSurface9* pScene = nullptr;
    IDirect3DSurface9* pDepthSurface = nullptr;
    IDirect3DTexture9* pDepth = nullptr;
    D3DSURFACE_DESC SceneDesc{};
    D3DSURFACE_DESC DepthDesc{};

    IDirect3DSurface9* pSavedTargets[nMaxTargets]{};
    IDirect3DSurface9* pSavedDepthStencil = nullptr;
    IDirect3DStateBlock9* pStateBlock = nullptr;

    IDirect3DVertexShader9* pVertexShader = nullptr;
    IDirect3DPixelShader9* pShaders[static_cast<size_t>(VolShader::Count)]{};
    Target Targets[static_cast<size_t>(VolTexture::Count)];

    IDirect3DSurface9* pProbeTarget = nullptr;
    IDirect3DSurface9* pProbeCopy = nullptr;
    D3DFORMAT eProbeFormat = D3DFMT_UNKNOWN;

    bool bSetupFailed = false;
    std::string szStatus;

    bool EnsureSetup()
    {
        if (pVertexShader && pStateBlock)
            return true;
        if (bSetupFailed)
            return false;
        bSetupFailed = true;

        std::vector<uint8_t> vCode;
        if (!CompileShader("FullscreenVS", "vs_3_0", { "VERTEX" }, vCode) ||
            FAILED(pDevice->CreateVertexShader(reinterpret_cast<const DWORD*>(vCode.data()), &pVertexShader)))
            return false;

        for (size_t i = 0; i < ShaderDefines.size(); i++)
        {
            vCode.clear();
            if (!CompileShader("main", "ps_3_0", { ShaderDefines[i] }, vCode) ||
                FAILED(pDevice->CreatePixelShader(reinterpret_cast<const DWORD*>(vCode.data()), &pShaders[i])))
                return false;
        }

        if (FAILED(pDevice->CreateStateBlock(D3DSBT_ALL, &pStateBlock)))
            return false;

        bSetupFailed = false;
        return true;
    }

    bool BindDepth(uintptr_t nDepthTexture)
    {
        std::string where;
        auto* pFound = FindD3DObject(nDepthTexture, VTableModule(reinterpret_cast<uintptr_t>(pScene)),
                                     __uuidof(IDirect3DTexture9), where);
        pDepth = static_cast<IDirect3DTexture9*>(pFound);
        if (!pDepth || FAILED(pDepth->GetSurfaceLevel(0, &pDepthSurface)) || FAILED(pDepthSurface->GetDesc(&DepthDesc)))
        {
            szStatus = "no depth texture in the DepthVPSampler object";
            return false;
        }

        szStatus = std::format("depth texture {}x{} format {} at {}", DepthDesc.Width, DepthDesc.Height,
                               static_cast<uint32_t>(DepthDesc.Format), where);
        return IsPackedDepth(DepthDesc.Format) || IsFloatDepth(DepthDesc.Format);
    }

    void SaveTargets()
    {
        for (DWORD i = 0; i < nMaxTargets; i++)
            pDevice->GetRenderTarget(i, &pSavedTargets[i]);
        pDevice->GetDepthStencilSurface(&pSavedDepthStencil);
    }

    void RestoreTargets()
    {
        // Before the state block: SetRenderTarget resets the viewport the block puts back.
        for (DWORD i = 0; i < nMaxTargets; i++)
        {
            pDevice->SetRenderTarget(i, pSavedTargets[i]);
            SafeRelease(pSavedTargets[i]);
        }
        pDevice->SetDepthStencilSurface(pSavedDepthStencil);
        SafeRelease(pSavedDepthStencil);
    }

    void SetCommonState()
    {
        pDevice->SetDepthStencilSurface(nullptr);
        pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);

        for (DWORD i = 0; i < 2; i++)
        {
            pDevice->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            pDevice->SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            pDevice->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            pDevice->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            pDevice->SetSamplerState(i, D3DSAMP_SRGBTEXTURE, FALSE);
        }

        pDevice->SetVertexShader(pVertexShader);
        pDevice->SetFVF(D3DFVF_XYZ);
    }

    void SetBlend(VolBlend eBlend)
    {
        if (eBlend == VolBlend::Replace)
        {
            pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            return;
        }

        // dest * a + src, the scene's alpha left alone.
        pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
        pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                        D3DCOLORWRITEENABLE_BLUE);
    }

    void SetOutput(VolTexture eOutput, VolSize& size)
    {
        auto* pTarget = pScene;
        size = SceneSize();
        if (eOutput != VolTexture::None)
        {
            const auto& target = Targets[static_cast<size_t>(eOutput)];
            pTarget = target.pSurf;
            size = target.size;
        }

        pDevice->SetRenderTarget(0, pTarget);
        for (DWORD i = 1; i < nMaxTargets; i++)
            pDevice->SetRenderTarget(i, nullptr);

        const D3DVIEWPORT9 viewport = { 0, 0, size.w, size.h, 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);
    }

    void ReleaseFrame()
    {
        SafeRelease(pDepthSurface);
        SafeRelease(pDepth);
        SafeRelease(pDevice);
        pScene = nullptr;
    }

    bool EnsureProbe()
    {
        if (pProbeTarget && pProbeCopy && eProbeFormat == DepthDesc.Format)
            return true;

        SafeRelease(pProbeTarget);
        SafeRelease(pProbeCopy);
        eProbeFormat = DepthDesc.Format;
        return SUCCEEDED(pDevice->CreateRenderTarget(1, 1, DepthDesc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                     &pProbeTarget, nullptr)) &&
               SUCCEEDED(pDevice->CreateOffscreenPlainSurface(1, 1, DepthDesc.Format, D3DPOOL_SYSTEMMEM,
                                                              &pProbeCopy, nullptr));
    }

    static void DecodeTexel(const uint8_t* pTexel, D3DFORMAT eFormat, float rgb[3])
    {
        if (IsPackedDepth(eFormat))
        {
            // BGRA in memory.
            rgb[0] = pTexel[2] / 255.0f;
            rgb[1] = pTexel[1] / 255.0f;
            rgb[2] = pTexel[0] / 255.0f;
        }
        else if (eFormat == D3DFMT_R16F || eFormat == D3DFMT_G16R16F || eFormat == D3DFMT_A16B16G16R16F)
            rgb[0] = HalfToFloat(*reinterpret_cast<const uint16_t*>(pTexel));
        else
            std::memcpy(&rgb[0], pTexel, sizeof(float));
    }

public:
    bool Begin(uintptr_t nSceneTarget, uintptr_t nDepthTexture) override
    {
        pScene = ReadPointer<IDirect3DSurface9*>(nSceneTarget + nTargetSurface);
        if (!pScene || FAILED(pScene->GetDesc(&SceneDesc)) || FAILED(pScene->GetDevice(&pDevice)))
        {
            szStatus = "no scene surface";
            ReleaseFrame();
            return false;
        }

        if (!BindDepth(nDepthTexture) || !EnsureSetup())
        {
            if (bSetupFailed)
                szStatus = "shader setup failed";
            ReleaseFrame();
            return false;
        }

        SaveTargets();
        pStateBlock->Capture();
        SetCommonState();
        return true;
    }

    void End() override
    {
        pDevice->SetTexture(0, nullptr);
        pDevice->SetTexture(1, nullptr);
        RestoreTargets();
        pStateBlock->Apply();
        ReleaseFrame();
    }

    VolSize SceneSize() const override { return { SceneDesc.Width, SceneDesc.Height }; }

    bool DepthPacked() const override { return IsPackedDepth(DepthDesc.Format); }

    bool EnsureTarget(VolTexture eTexture, VolSize size) override
    {
        return Targets[static_cast<size_t>(eTexture)].Ensure(pDevice, LowResSize(eTexture, size));
    }

    void Draw(const VolDraw& draw, const VolConstants& constants) override
    {
        VolSize size{};
        SetOutput(draw.eOutput, size);
        SetBlend(draw.eBlend);

        pDevice->SetTexture(0, pDepth);
        auto* pInput = draw.eInput == VolTexture::None ? nullptr : Targets[static_cast<size_t>(draw.eInput)].pTex;
        pDevice->SetTexture(1, pInput);

        // D3D9 samples texel corners; half a pixel moves the triangle onto centres.
        const float fHalfPixel[4] = { -1.0f / size.w, 1.0f / size.h, 0.0f, 0.0f };
        pDevice->SetVertexShaderConstantF(0, fHalfPixel, 1);
        pDevice->SetPixelShaderConstantF(0, &constants.c[0][0], nVolRegisters);
        pDevice->SetPixelShader(pShaders[static_cast<size_t>(draw.eShader)]);

        const float fTriangle[3][3] = { { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f }, { 0.0f, 2.0f, 0.0f } };
        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, fTriangle, sizeof(fTriangle[0]));

        pDevice->SetTexture(1, nullptr);
    }

    bool ReadDepthCentre(float rgb[3]) override
    {
        if (!EnsureProbe())
            return false;

        const RECT centre = { static_cast<LONG>(DepthDesc.Width / 2), static_cast<LONG>(DepthDesc.Height / 2),
                              static_cast<LONG>(DepthDesc.Width / 2 + 1), static_cast<LONG>(DepthDesc.Height / 2 + 1) };
        if (FAILED(pDevice->StretchRect(pDepthSurface, &centre, pProbeTarget, nullptr, D3DTEXF_POINT)) ||
            FAILED(pDevice->GetRenderTargetData(pProbeTarget, pProbeCopy)))
            return false;

        D3DLOCKED_RECT locked{};
        if (FAILED(pProbeCopy->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            return false;

        DecodeTexel(static_cast<const uint8_t*>(locked.pBits), DepthDesc.Format, rgb);
        pProbeCopy->UnlockRect();
        return true;
    }

    const std::string& Status() const override { return szStatus; }

    void Release() override
    {
        for (auto& target : Targets)
            target.Release();
        SafeRelease(pProbeTarget);
        SafeRelease(pProbeCopy);
        SafeRelease(pStateBlock);
        SafeRelease(pVertexShader);
        for (auto*& pShader : pShaders)
            SafeRelease(pShader);
        bSetupFailed = false;
    }
};

}

// Direct3D 10

namespace dx10
{

// Engine render target object +0x08, its texture wrapper, as FUN_10430250 reads it.
static constexpr uintptr_t nTargetWrapper = 0x08;
static constexpr uintptr_t nWrapperTargetView = 0x1C;

static constexpr UINT nMaxTargets = D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT;
static constexpr UINT nSavedSlots = 2;

struct Target
{
    ID3D10Texture2D* pTex = nullptr;
    ID3D10RenderTargetView* pRtv = nullptr;
    ID3D10ShaderResourceView* pSrv = nullptr;
    VolSize size{};

    void Release()
    {
        SafeRelease(pSrv);
        SafeRelease(pRtv);
        SafeRelease(pTex);
        size = {};
    }

    bool Ensure(ID3D10Device* pDevice, VolSize wanted)
    {
        if (pRtv && size.w == wanted.w && size.h == wanted.h)
            return true;

        Release();
        D3D10_TEXTURE2D_DESC desc{};
        desc.Width = wanted.w;
        desc.Height = wanted.h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D10_USAGE_DEFAULT;
        desc.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;

        if (FAILED(pDevice->CreateTexture2D(&desc, nullptr, &pTex)) ||
            FAILED(pDevice->CreateRenderTargetView(pTex, nullptr, &pRtv)) ||
            FAILED(pDevice->CreateShaderResourceView(pTex, nullptr, &pSrv)))
        {
            Release();
            return false;
        }

        size = wanted;
        return true;
    }
};

// What the passes change, read back before and set again after.
struct SavedState
{
    ID3D10RenderTargetView* pTargets[nMaxTargets]{};
    ID3D10DepthStencilView* pDepthStencil = nullptr;
    ID3D10BlendState* pBlend = nullptr;
    float fBlendFactor[4]{};
    UINT nSampleMask = 0;
    ID3D10DepthStencilState* pDepthState = nullptr;
    UINT nStencilRef = 0;
    ID3D10RasterizerState* pRasterizer = nullptr;
    D3D10_VIEWPORT Viewports[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT nViewports = 0;
    ID3D10VertexShader* pVertexShader = nullptr;
    ID3D10GeometryShader* pGeometryShader = nullptr;
    ID3D10PixelShader* pPixelShader = nullptr;
    ID3D10ShaderResourceView* pResources[nSavedSlots]{};
    ID3D10SamplerState* pSamplers[nSavedSlots]{};
    ID3D10Buffer* pConstants = nullptr;
    ID3D10InputLayout* pLayout = nullptr;
    D3D10_PRIMITIVE_TOPOLOGY eTopology = D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED;

    void Save(ID3D10Device* pDevice)
    {
        pDevice->OMGetRenderTargets(nMaxTargets, pTargets, &pDepthStencil);
        pDevice->OMGetBlendState(&pBlend, fBlendFactor, &nSampleMask);
        pDevice->OMGetDepthStencilState(&pDepthState, &nStencilRef);
        pDevice->RSGetState(&pRasterizer);
        nViewports = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        pDevice->RSGetViewports(&nViewports, Viewports);
        pDevice->VSGetShader(&pVertexShader);
        pDevice->GSGetShader(&pGeometryShader);
        pDevice->PSGetShader(&pPixelShader);
        pDevice->PSGetShaderResources(0, nSavedSlots, pResources);
        pDevice->PSGetSamplers(0, nSavedSlots, pSamplers);
        pDevice->PSGetConstantBuffers(0, 1, &pConstants);
        pDevice->IAGetInputLayout(&pLayout);
        pDevice->IAGetPrimitiveTopology(&eTopology);
    }

    void Restore(ID3D10Device* pDevice)
    {
        pDevice->OMSetRenderTargets(nMaxTargets, pTargets, pDepthStencil);
        pDevice->OMSetBlendState(pBlend, fBlendFactor, nSampleMask);
        pDevice->OMSetDepthStencilState(pDepthState, nStencilRef);
        pDevice->RSSetState(pRasterizer);
        pDevice->RSSetViewports(nViewports, Viewports);
        pDevice->VSSetShader(pVertexShader);
        pDevice->GSSetShader(pGeometryShader);
        pDevice->PSSetShader(pPixelShader);
        pDevice->PSSetShaderResources(0, nSavedSlots, pResources);
        pDevice->PSSetSamplers(0, nSavedSlots, pSamplers);
        pDevice->PSSetConstantBuffers(0, 1, &pConstants);
        pDevice->IASetInputLayout(pLayout);
        pDevice->IASetPrimitiveTopology(eTopology);
        ReleaseAll();
    }

    void ReleaseAll()
    {
        for (auto*& pTarget : pTargets)
            SafeRelease(pTarget);
        SafeRelease(pDepthStencil);
        SafeRelease(pBlend);
        SafeRelease(pDepthState);
        SafeRelease(pRasterizer);
        SafeRelease(pVertexShader);
        SafeRelease(pGeometryShader);
        SafeRelease(pPixelShader);
        for (auto*& pResource : pResources)
            SafeRelease(pResource);
        for (auto*& pSampler : pSamplers)
            SafeRelease(pSampler);
        SafeRelease(pConstants);
        SafeRelease(pLayout);
    }
};

static DXGI_FORMAT DepthViewFormat(DXGI_FORMAT eFormat)
{
    switch (eFormat)
    {
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

class Device final : public VolumetricDevice
{
    ID3D10Device* pDevice = nullptr;
    ID3D10RenderTargetView* pScene = nullptr;
    D3D10_TEXTURE2D_DESC SceneDesc{};

    // Kept across frames while the engine keeps the same depth texture.
    ID3D10Texture2D* pDepthTexture = nullptr;
    ID3D10ShaderResourceView* pDepth = nullptr;
    D3D10_TEXTURE2D_DESC DepthDesc{};
    DXGI_FORMAT eDepthView = DXGI_FORMAT_UNKNOWN;

    SavedState Saved;
    ID3D10VertexShader* pVertexShader = nullptr;
    ID3D10PixelShader* pShaders[static_cast<size_t>(VolShader::Count)]{};
    ID3D10BlendState* pBlendReplace = nullptr;
    ID3D10BlendState* pBlendOver = nullptr;
    ID3D10DepthStencilState* pNoDepth = nullptr;
    ID3D10RasterizerState* pRasterizer = nullptr;
    ID3D10SamplerState* pPoint = nullptr;
    ID3D10Buffer* pConstants = nullptr;
    Target Targets[static_cast<size_t>(VolTexture::Count)];
    ID3D10Texture2D* pProbe = nullptr;

    bool bSetupFailed = false;
    std::string szStatus;

    bool CreateShaders()
    {
        std::vector<uint8_t> vCode;
        if (!CompileShader("FullscreenVS", "vs_4_0", { "VERTEX", "SM4" }, vCode) ||
            FAILED(pDevice->CreateVertexShader(vCode.data(), vCode.size(), &pVertexShader)))
            return false;

        for (size_t i = 0; i < ShaderDefines.size(); i++)
        {
            vCode.clear();
            if (!CompileShader("main", "ps_4_0", { ShaderDefines[i], "SM4" }, vCode) ||
                FAILED(pDevice->CreatePixelShader(vCode.data(), vCode.size(), &pShaders[i])))
                return false;
        }
        return true;
    }

    bool CreateBlendStates()
    {
        D3D10_BLEND_DESC blend{};
        blend.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(pDevice->CreateBlendState(&blend, &pBlendReplace)))
            return false;

        // dest * a + src, the scene's alpha left alone.
        blend.BlendEnable[0] = TRUE;
        blend.SrcBlend = D3D10_BLEND_ONE;
        blend.DestBlend = D3D10_BLEND_SRC_ALPHA;
        blend.BlendOp = D3D10_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D10_BLEND_ZERO;
        blend.DestBlendAlpha = D3D10_BLEND_ONE;
        blend.BlendOpAlpha = D3D10_BLEND_OP_ADD;
        blend.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_RED | D3D10_COLOR_WRITE_ENABLE_GREEN |
                                         D3D10_COLOR_WRITE_ENABLE_BLUE;
        return SUCCEEDED(pDevice->CreateBlendState(&blend, &pBlendOver));
    }

    bool CreateFixedStates()
    {
        D3D10_DEPTH_STENCIL_DESC depth{};
        D3D10_RASTERIZER_DESC raster{};
        raster.FillMode = D3D10_FILL_SOLID;
        raster.CullMode = D3D10_CULL_NONE;
        raster.DepthClipEnable = TRUE;

        D3D10_SAMPLER_DESC sampler{};
        sampler.Filter = D3D10_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D10_FLOAT32_MAX;

        D3D10_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(VolConstants);
        buffer.Usage = D3D10_USAGE_DYNAMIC;
        buffer.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

        return SUCCEEDED(pDevice->CreateDepthStencilState(&depth, &pNoDepth)) &&
               SUCCEEDED(pDevice->CreateRasterizerState(&raster, &pRasterizer)) &&
               SUCCEEDED(pDevice->CreateSamplerState(&sampler, &pPoint)) &&
               SUCCEEDED(pDevice->CreateBuffer(&buffer, nullptr, &pConstants));
    }

    bool EnsureSetup()
    {
        if (pConstants)
            return true;
        if (bSetupFailed)
            return false;

        bSetupFailed = !CreateShaders() || !CreateBlendStates() || !CreateFixedStates();
        return !bSetupFailed;
    }

    bool ReadScene(uintptr_t nSceneTarget)
    {
        const auto nWrapper = ReadPointer<uintptr_t>(nSceneTarget + nTargetWrapper);
        pScene = nWrapper ? ReadPointer<ID3D10RenderTargetView*>(nWrapper + nWrapperTargetView) : nullptr;
        if (!pScene)
            return false;

        ID3D10Resource* pResource = nullptr;
        ID3D10Texture2D* pTexture = nullptr;
        pScene->GetResource(&pResource);
        if (pResource)
            pResource->QueryInterface(__uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&pTexture));
        if (pTexture)
            pTexture->GetDesc(&SceneDesc);

        SafeRelease(pTexture);
        SafeRelease(pResource);
        pScene->GetDevice(&pDevice);
        return pDevice != nullptr && SceneDesc.Width != 0;
    }

    // Held by the view kept across frames, so the address cannot be reused while it is cached.
    bool BindDepth(uintptr_t nDepthTexture)
    {
        std::string where;
        auto* pFound = FindD3DObject(nDepthTexture, VTableModule(reinterpret_cast<uintptr_t>(pScene)),
                                     __uuidof(ID3D10Texture2D), where);
        auto* pTexture = static_cast<ID3D10Texture2D*>(pFound);
        if (!pTexture)
        {
            szStatus = "no depth texture in the DepthVPSampler object";
            return false;
        }

        // The view holds its own reference.
        pTexture->Release();
        if (pTexture == pDepthTexture && pDepth)
            return true;

        SafeRelease(pDepth);
        SafeRelease(pProbe);
        pDepthTexture = pTexture;
        pTexture->GetDesc(&DepthDesc);
        eDepthView = DepthViewFormat(DepthDesc.Format);
        szStatus = std::format("depth texture {}x{} format {} bind {:X} at {}", DepthDesc.Width, DepthDesc.Height,
                               static_cast<uint32_t>(DepthDesc.Format), DepthDesc.BindFlags, where);

        if (eDepthView == DXGI_FORMAT_UNKNOWN || !(DepthDesc.BindFlags & D3D10_BIND_SHADER_RESOURCE))
            return false;

        D3D10_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = eDepthView;
        view.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        return SUCCEEDED(pDevice->CreateShaderResourceView(pTexture, &view, &pDepth));
    }

    void SetCommonState()
    {
        pDevice->OMSetDepthStencilState(pNoDepth, 0);
        pDevice->RSSetState(pRasterizer);
        pDevice->IASetInputLayout(nullptr);
        pDevice->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        pDevice->VSSetShader(pVertexShader);
        pDevice->GSSetShader(nullptr);
        pDevice->PSSetConstantBuffers(0, 1, &pConstants);

        ID3D10SamplerState* pSamplers[nSavedSlots] = { pPoint, pPoint };
        pDevice->PSSetSamplers(0, nSavedSlots, pSamplers);
    }

    VolSize SetOutput(VolTexture eOutput)
    {
        auto* pTarget = pScene;
        auto size = SceneSize();
        if (eOutput != VolTexture::None)
        {
            const auto& target = Targets[static_cast<size_t>(eOutput)];
            pTarget = target.pRtv;
            size = target.size;
        }

        pDevice->OMSetRenderTargets(1, &pTarget, nullptr);
        const D3D10_VIEWPORT viewport = { 0, 0, size.w, size.h, 0.0f, 1.0f };
        pDevice->RSSetViewports(1, &viewport);
        return size;
    }

    void UploadConstants(const VolConstants& constants)
    {
        void* pData = nullptr;
        if (FAILED(pConstants->Map(D3D10_MAP_WRITE_DISCARD, 0, &pData)))
            return;
        std::memcpy(pData, &constants, sizeof(constants));
        pConstants->Unmap();
    }

    void UnbindResources()
    {
        ID3D10ShaderResourceView* pNone[nSavedSlots]{};
        pDevice->PSSetShaderResources(0, nSavedSlots, pNone);
    }

    void ReleaseFrame()
    {
        SafeRelease(pDevice);
        pScene = nullptr;
    }

    bool EnsureProbe()
    {
        if (pProbe)
            return true;

        D3D10_TEXTURE2D_DESC desc = DepthDesc;
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D10_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        return SUCCEEDED(pDevice->CreateTexture2D(&desc, nullptr, &pProbe));
    }

    void DecodeTexel(const uint8_t* pTexel, float rgb[3]) const
    {
        if (eDepthView == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            for (size_t i = 0; i < 3; i++)
                rgb[i] = pTexel[i] / 255.0f;
        }
        else if (eDepthView == DXGI_FORMAT_R16_FLOAT || eDepthView == DXGI_FORMAT_R16G16B16A16_FLOAT)
            rgb[0] = HalfToFloat(*reinterpret_cast<const uint16_t*>(pTexel));
        else
            std::memcpy(&rgb[0], pTexel, sizeof(float));
    }

public:
    bool Begin(uintptr_t nSceneTarget, uintptr_t nDepthTexture) override
    {
        if (!ReadScene(nSceneTarget))
            szStatus = "no scene target view";
        else if (!EnsureSetup())
            szStatus = "shader setup failed";

        if (!pDevice || bSetupFailed || !BindDepth(nDepthTexture))
        {
            ReleaseFrame();
            return false;
        }

        Saved.Save(pDevice);
        SetCommonState();
        return true;
    }

    void End() override
    {
        UnbindResources();
        Saved.Restore(pDevice);
        ReleaseFrame();
    }

    VolSize SceneSize() const override { return { SceneDesc.Width, SceneDesc.Height }; }

    bool DepthPacked() const override { return eDepthView == DXGI_FORMAT_R8G8B8A8_UNORM; }

    bool EnsureTarget(VolTexture eTexture, VolSize size) override
    {
        return Targets[static_cast<size_t>(eTexture)].Ensure(pDevice, LowResSize(eTexture, size));
    }

    void Draw(const VolDraw& draw, const VolConstants& constants) override
    {
        UnbindResources();
        SetOutput(draw.eOutput);
        const float fFactor[4]{};
        pDevice->OMSetBlendState(draw.eBlend == VolBlend::Over ? pBlendOver : pBlendReplace, fFactor, 0xFFFFFFFF);

        UploadConstants(constants);
        auto* pInput = draw.eInput == VolTexture::None ? nullptr : Targets[static_cast<size_t>(draw.eInput)].pSrv;
        ID3D10ShaderResourceView* pResources[nSavedSlots] = { pDepth, pInput };
        pDevice->PSSetShaderResources(0, nSavedSlots, pResources);
        pDevice->PSSetShader(pShaders[static_cast<size_t>(draw.eShader)]);
        pDevice->Draw(3, 0);
    }

    bool ReadDepthCentre(float rgb[3]) override
    {
        if (!EnsureProbe())
            return false;

        const D3D10_BOX centre = { DepthDesc.Width / 2, DepthDesc.Height / 2, 0,
                                   DepthDesc.Width / 2 + 1, DepthDesc.Height / 2 + 1, 1 };
        pDevice->CopySubresourceRegion(pProbe, 0, 0, 0, 0, pDepthTexture, 0, &centre);

        D3D10_MAPPED_TEXTURE2D mapped{};
        if (FAILED(pProbe->Map(0, D3D10_MAP_READ, 0, &mapped)))
            return false;

        DecodeTexel(static_cast<const uint8_t*>(mapped.pData), rgb);
        pProbe->Unmap(0);
        return true;
    }

    const std::string& Status() const override { return szStatus; }

    void Release() override
    {
        for (auto& target : Targets)
            target.Release();
        SafeRelease(pProbe);
        SafeRelease(pDepth);
        pDepthTexture = nullptr;
        SafeRelease(pConstants);
        SafeRelease(pPoint);
        SafeRelease(pRasterizer);
        SafeRelease(pNoDepth);
        SafeRelease(pBlendOver);
        SafeRelease(pBlendReplace);
        SafeRelease(pVertexShader);
        for (auto*& pShader : pShaders)
            SafeRelease(pShader);
        bSetupFailed = false;
    }
};

}

export VolumetricDevice& VolumetricDeviceDX9()
{
    static dx9::Device device;
    return device;
}

export VolumetricDevice& VolumetricDeviceDX10()
{
    static dx10::Device device;
    return device;
}
