module;

#include <common.hxx>

export module aniso;

import common;
import dunia;
import settings;

// Dunia parses SamplerStates.xml into raw D3D9 sampler descriptors. Patching the parsed
// descriptors covers every quality level without replacing game files. Selection is by rule
// rather than sampler name; render-target and point-filter samplers do not use mipmapped
// filtering and are skipped.

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

// MaxAnisotropy queried by the loader, or UINT_MAX before a device exists.
static void ApplyAnisotropy(SamplerDesc* pDesc, uint32_t nDeviceMax)
{
    if (nAnisotropicFiltering < 2)
        return;

    if (pDesc->nMipFilter == nFilterNone)
        return;

    if (pDesc->nMinFilter == nFilterPoint || pDesc->nMagFilter == nFilterPoint)
        return;

    // magfilter stays linear. Anisotropy is a minification filter; in the mag slot it only costs
    // sampler bandwidth.
    pDesc->nMinFilter = nFilterAnisotropic;
    pDesc->nMaxAnisotropy = nAnisotropicFiltering < nDeviceMax ? nAnisotropicFiltering : nDeviceMax;
}

class AnisotropicFiltering
{
public:
    AnisotropicFiltering()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Hook after XML parsing so the descriptor is fully populated and device caps are available.
            auto pattern = dunia_pattern("8B 4C 24 18 85 C9 74 07 8B 01 8B 50 08 FF D2 8B 06 8B 50 08 8B CE FF D2 5F 5E 5D 5B 81");
            if (pattern.empty())
                return;

            static auto SamplerStateHook = safetyhook::create_mid(pattern.get_first(15), [](SafetyHookContext& regs)
            {
                ApplyAnisotropy((SamplerDesc*)regs.ebx, *(uint32_t*)(regs.esp + 0x20));
            });

            static auto AnisotropicFilteringCB = []()
            {
                nAnisotropicFiltering = static_cast<uint32_t>(JackalFixSettings.GetInt(PREF_ANISOTROPICFILTERING));
            };

            AnisotropicFilteringCB();

            // Samplers load once at startup. Changes apply on the next launch.
            JackalFix::onIniFileChange() += []()
            {
                AnisotropicFilteringCB();
            };
        };
    }
} AnisotropicFiltering;
