module;

#include <common.hxx>
#include <objbase.h> // common.hxx defines WIN32_LEAN_AND_MEAN, so the COM types are not in scope

export module systemdetection;

import common;
import settings;

// DxDiag is required during startup, so cache its results rather than fail it. WMI queries can be
// skipped; their consumers handle missing data.

static constexpr GUID guidDxDiagProvider = { 0xA65B8071, 0x3BFE, 0x4213, { 0x9A, 0x5B, 0x49, 0x1D, 0xA4, 0x46, 0x1C, 0xA7 } };
static constexpr GUID guidIDxDiagProvider = { 0x9C6B4CB0, 0x23F8, 0x49CC, { 0xA3, 0xED, 0x45, 0xA5, 0x50, 0x00, 0xA6, 0xD2 } };
static constexpr GUID guidWbemLocator = { 0x4590F811, 0x1D3A, 0x11D0, { 0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } };

// Minimal IDxDiagProvider declaration to avoid depending on dxdiag.h.
struct DxDiagProvider;
struct DxDiagProviderVtbl
{
    HRESULT(__stdcall* QueryInterface)(DxDiagProvider*, REFIID, void**);
    ULONG(__stdcall* AddRef)(DxDiagProvider*);
    ULONG(__stdcall* Release)(DxDiagProvider*);
    HRESULT(__stdcall* Initialize)(DxDiagProvider*, void*);
    HRESULT(__stdcall* GetRootContainer)(DxDiagProvider*, void**);
};
struct DxDiagProvider { const DxDiagProviderVtbl* lpVtbl; };

static bool bSkipSystemDetection = true;

static DxDiagProvider* pRealProvider = nullptr;
static IUnknown* pCachedRoot = nullptr;
static bool bProviderInitialised = false;
static LONG nProxyRefs = 0;

static HRESULT __stdcall ProxyQueryInterface(DxDiagProvider* pThis, REFIID riid, void** ppvObject)
{
    if (!ppvObject)
        return E_POINTER;

    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, guidIDxDiagProvider))
    {
        InterlockedIncrement(&nProxyRefs);
        *ppvObject = pThis;
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

static ULONG __stdcall ProxyAddRef(DxDiagProvider*)
{
    return (ULONG)InterlockedIncrement(&nProxyRefs);
}

// Keep the real provider alive so later callers reuse the initialized instance.
static ULONG __stdcall ProxyRelease(DxDiagProvider*)
{
    auto n = InterlockedDecrement(&nProxyRefs);
    return n > 0 ? (ULONG)n : 0;
}

static HRESULT __stdcall ProxyInitialize(DxDiagProvider*, void* pParams)
{
    if (bProviderInitialised)
        return S_OK;

    auto hr = pRealProvider->lpVtbl->Initialize(pRealProvider, pParams);
    bProviderInitialised = SUCCEEDED(hr);
    return hr;
}

static HRESULT __stdcall ProxyGetRootContainer(DxDiagProvider*, void** ppInstance)
{
    if (!ppInstance)
        return E_POINTER;

    if (!pCachedRoot)
    {
        auto hr = pRealProvider->lpVtbl->GetRootContainer(pRealProvider, (void**)&pCachedRoot);
        if (FAILED(hr))
            return hr;
    }

    // Handed out with a reference of its own, so each caller's Release stays balanced.
    pCachedRoot->AddRef();
    *ppInstance = pCachedRoot;
    return S_OK;
}

static const DxDiagProviderVtbl ProxyVtbl =
{
    ProxyQueryInterface, ProxyAddRef, ProxyRelease, ProxyInitialize, ProxyGetRootContainer
};
static DxDiagProvider ProxyProvider = { &ProxyVtbl };

using CoCreateInstance_t = HRESULT(__stdcall*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static CoCreateInstance_t pfnCoCreateInstance = nullptr;

static HRESULT __stdcall CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv)
{
    if (bSkipSystemDetection && ppv)
    {
        if (IsEqualGUID(rclsid, guidWbemLocator))
            return REGDB_E_CLASSNOTREG;

        if (IsEqualGUID(rclsid, guidDxDiagProvider) && IsEqualGUID(riid, guidIDxDiagProvider))
        {
            if (!pRealProvider)
            {
                auto hr = pfnCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, (void**)&pRealProvider);
                if (FAILED(hr))
                {
                    pRealProvider = nullptr;
                    return hr;
                }
            }

            InterlockedIncrement(&nProxyRefs);
            *ppv = &ProxyProvider;
            return S_OK;
        }
    }

    return pfnCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}

// Redirects an imported function in one module's IAT, so only that module's calls are affected.
static bool HookImport(HMODULE hModule, const char* szModule, const char* szFunction, void* pDetour, void** ppOriginal)
{
    if (!hModule)
        return false;

    auto pBase = (uint8_t*)hModule;
    auto pDos = (IMAGE_DOS_HEADER*)pBase;
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto pNt = (IMAGE_NT_HEADERS*)(pBase + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    auto& dir = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return false;

    for (auto pImport = (IMAGE_IMPORT_DESCRIPTOR*)(pBase + dir.VirtualAddress); pImport->Name; ++pImport)
    {
        if (lstrcmpiA((const char*)(pBase + pImport->Name), szModule) != 0)
            continue;

        // OriginalFirstThunk contains import names. Use FirstThunk when it is missing.
        auto pThunk = (IMAGE_THUNK_DATA*)(pBase + pImport->FirstThunk);
        auto pNames = (IMAGE_THUNK_DATA*)(pBase + (pImport->OriginalFirstThunk ? pImport->OriginalFirstThunk : pImport->FirstThunk));

        for (; pNames->u1.AddressOfData; ++pNames, ++pThunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL(pNames->u1.Ordinal))
                continue;

            auto pName = (IMAGE_IMPORT_BY_NAME*)(pBase + pNames->u1.AddressOfData);
            if (strcmp((const char*)pName->Name, szFunction) != 0)
                continue;

            if (ppOriginal)
                *ppOriginal = (void*)pThunk->u1.Function;

            DWORD nOldProtect = 0;
            if (!VirtualProtect(&pThunk->u1.Function, sizeof(pThunk->u1.Function), PAGE_READWRITE, &nOldProtect))
                return false;

            pThunk->u1.Function = (uintptr_t)pDetour;
            VirtualProtect(&pThunk->u1.Function, sizeof(pThunk->u1.Function), nOldProtect, &nOldProtect);
            return true;
        }
    }

    return false;
}

class SystemDetection
{
public:
    SystemDetection()
    {
        // Hooked on load; Dunia does not load this DLL until InitDuniaEngine is already running.
        CallbackHandler::RegisterCallback(L"systemdetection.dll", []()
        {
            bSkipSystemDetection = JackalFixSettings.GetInt(PREF_SKIPSYSTEMDETECTION) != 0;

            auto hModule = GetModuleHandleW(L"systemdetection.dll");
            HookImport(hModule, "ole32.dll", "CoCreateInstance", CoCreateInstanceHook, (void**)&pfnCoCreateInstance);

            // Detection only runs during startup, so a live ini change takes effect next launch.
            JackalFix::onIniFileChange() += []()
            {
                bSkipSystemDetection = JackalFixSettings.GetInt(PREF_SKIPSYSTEMDETECTION) != 0;
            };
        });
    }
} SystemDetection;
