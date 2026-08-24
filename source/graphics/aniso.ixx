module;

#include <common.hxx>

export module aniso;

import common;
import dunia;
import settings;

static constexpr uint32_t nFilterNone = 0;
static constexpr uint32_t nFilterPoint = 1;
static constexpr uint32_t nFilterAnisotropic = 3;

struct SamplerDesc
{
    void* pVTable;              // 0x00
    uint8_t bDeclared;          // 0x04  set only for quality levels the XML actually declares
    uint8_t bPad[3];            // 0x05
    uint32_t nAddressU;         // 0x08
    uint32_t nAddressV;         // 0x0C
    uint32_t nAddressW;         // 0x10
    uint32_t nBorderColor;      // 0x14
    uint32_t nMagFilter;        // 0x18
    uint32_t nMinFilter;        // 0x1C
    uint32_t nMipFilter;        // 0x20
    float fMipMapLodBias;       // 0x24
    uint32_t nMaxMipLevel;      // 0x28
    uint32_t nMaxAnisotropy;    // 0x2C
    uint8_t bFetch4;            // 0x30
    uint8_t bFetch4Supported;   // 0x31
};

static_assert(offsetof(SamplerDesc, nMagFilter) == 0x18);
static_assert(offsetof(SamplerDesc, nMinFilter) == 0x1C);
static_assert(offsetof(SamplerDesc, nMipFilter) == 0x20);
static_assert(offsetof(SamplerDesc, nMaxAnisotropy) == 0x2C);

static uint32_t nAnisotropicFiltering = 0;

// Live changes: descriptors are parsed once but their fields are re-pushed into SetSamplerState on
// every bind, so editing one is enough provided the XML values are kept. The bind wrapper's
// redundancy table at manager+0xC4 blocks that until memset, as the engine does on sampler reload.
struct SamplerRecord
{
    SamplerDesc* pDesc;
    uint32_t nMinFilter;      // what the XML said, before anything of ours
    uint32_t nMaxAnisotropy;
    uint32_t nDeviceMax;
};

// Sized so a modified XML cannot walk off the end; anything past it keeps the value it was parsed with.
static constexpr size_t nMaxSamplers = 1024;
static SamplerRecord Samplers[nMaxSamplers]{};
static size_t nSamplers = 0;

// The sampler state manager, and the redundancy table inside it.
static constexpr ptrdiff_t nManagerBindShadow = 0xC4;
static constexpr size_t    nManagerShadowSize = 0x454;
static void** ppSamplerManager = nullptr;

// Selected by rule, not by sampler name.
// nDeviceMax is MaxAnisotropy queried by the loader, or UINT_MAX before a device exists.
static void ApplyAnisotropy(SamplerDesc* pDesc, uint32_t nDeviceMax)
{
    if (nAnisotropicFiltering < 2)
        return;

    if (pDesc->nMipFilter == nFilterNone)
        return;

    if (pDesc->nMinFilter == nFilterPoint || pDesc->nMagFilter == nFilterPoint)
        return;

    // magfilter stays linear: anisotropy is a minification filter, in the mag slot it only costs bandwidth.
    pDesc->nMinFilter = nFilterAnisotropic;
    pDesc->nMaxAnisotropy = nAnisotropicFiltering < nDeviceMax ? nAnisotropicFiltering : nDeviceMax;
}

// The parse runs once, so this is the only chance to record the XML values.
static void RecordSampler(SamplerDesc* pDesc, uint32_t nDeviceMax)
{
    if (pDesc == nullptr || nSamplers >= nMaxSamplers)
        return;

    for (size_t i = 0; i < nSamplers; i++)
    {
        if (Samplers[i].pDesc == pDesc)
            return;
    }

    Samplers[nSamplers++] = { pDesc, pDesc->nMinFilter, pDesc->nMaxAnisotropy, nDeviceMax };
}

// Restores the XML values, applies the current setting, then drops the bind shadow so each slot
// reprograms on its next bind.
static void ReapplyAnisotropy()
{
    size_t nChanged = 0;

    for (size_t i = 0; i < nSamplers; i++)
    {
        auto& record = Samplers[i];
        if (record.pDesc == nullptr)
            continue;

        record.pDesc->nMinFilter = record.nMinFilter;
        record.pDesc->nMaxAnisotropy = record.nMaxAnisotropy;
        ApplyAnisotropy(record.pDesc, record.nDeviceMax);

        if (record.pDesc->nMinFilter != record.nMinFilter
            || record.pDesc->nMaxAnisotropy != record.nMaxAnisotropy)
            nChanged++;
    }

    auto* pManager = ppSamplerManager != nullptr ? static_cast<uint8_t*>(*ppSamplerManager) : nullptr;
    if (pManager != nullptr)
        memset(pManager + nManagerBindShadow, 0, nManagerShadowSize);
}

class AnisotropicFiltering
{
public:
    AnisotropicFiltering()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Hooked after XML parsing: descriptor fully populated, device caps available. Every
            // quality level the XML declares comes through, so no game file is replaced.
            auto* pSamplerState = dunia_find("8B 4C 24 18 85 C9 74 07 8B 01 8B 50 08 FF D2 8B 06 8B 50 08 8B CE FF D2 5F 5E 5D 5B 81", 15);
            if (!pSamplerState)
                return;

            static auto SamplerStateHook = safetyhook::create_mid(pSamplerState, [](SafetyHookContext& regs)
            {
                auto* pDesc = reinterpret_cast<SamplerDesc*>(regs.ebx);
                const auto nDeviceMax = *reinterpret_cast<uint32_t*>(regs.esp + 0x20);

                RecordSampler(pDesc, nDeviceMax);
                ApplyAnisotropy(pDesc, nDeviceMax);
            });

            // Sampler state manager; the vertex sampler base 101h right behind it makes the site unique.
            if (auto* pManagerRef = dunia_find("8B 41 0C 85 C0 74 20 8B 4C 24 08 0F B7 11 8B 0D ? ? ? ? 81 C2 01 01 00 00", 16))
                ppSamplerManager = *static_cast<void***>(pManagerRef);

            BindInt(nAnisotropicFiltering, PREF_ANISOTROPICFILTERING);

            // Registered after the bind so the new value is in place before the reapply runs.
            JackalFix::onIniFileChange() += []()
            {
                ReapplyAnisotropy();
            };
        };
    }
} AnisotropicFiltering;
