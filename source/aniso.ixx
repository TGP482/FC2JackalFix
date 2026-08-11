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

// Changing this while the game runs.
//
// The descriptors are parsed once and never rebuilt, but they are not baked into anything: the
// sampler is programmed straight out of the descriptor every time it is bound. Dunia+419720 pushes
// [desc+08h], [desc+0Ch], [desc+10h], [desc+14h], [desc+18h], [desc+1Ch], [desc+20h], [desc+2Ch]
// and [desc+24h] into SetSamplerState one after another, all read at call time. So editing a
// descriptor is enough, as long as the descriptor is edited again when the setting moves, which
// means keeping what each one said before we touched it.
//
// The one thing in the way is a redundancy shadow. The bind wrapper at Dunia+415D70 compares the
// sampler set it is about to bind against a table at manager+0C4h and returns without programming
// anything when they match, so a slot already bound to that set never sees the new numbers. The
// engine clears that same table with a memset of 454h bytes when it reloads the samplers, and that
// is what is done here. Every slot then programs itself again on its next bind.
struct SamplerRecord
{
    SamplerDesc* pDesc;
    uint32_t nMinFilter;      // what the XML said, before anything of ours
    uint32_t nMaxAnisotropy;
    uint32_t nDeviceMax;
};

// The shipped SamplerStates.xml declares far fewer than this; the array is sized so a modified one
// cannot walk off the end, and anything past it keeps the value it was parsed with.
static constexpr size_t nMaxSamplers = 1024;
static SamplerRecord Samplers[nMaxSamplers]{};
static size_t nSamplers = 0;

// The sampler state manager, and the redundancy table inside it.
static constexpr ptrdiff_t nManagerBindShadow = 0xC4;
static constexpr size_t    nManagerShadowSize = 0x454;
static void** ppSamplerManager = nullptr;

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

// Remembers a descriptor the first time it is seen, so it can be put back later. The parse runs
// once, so this is the only chance to read what the file actually said.
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

// Puts every descriptor back to what the file said and applies the setting as it now stands, then
// drops the bind shadow so the next bind of each slot programs the sampler again.
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
            // Hooked after XML parsing, so the descriptor is fully populated and device caps
            // are available.
            auto pattern = dunia_pattern("8B 4C 24 18 85 C9 74 07 8B 01 8B 50 08 FF D2 8B 06 8B 50 08 8B CE FF D2 5F 5E 5D 5B 81");
            if (pattern.empty())
                return;

            static auto SamplerStateHook = safetyhook::create_mid(pattern.get_first(15), [](SafetyHookContext& regs)
            {
                auto* pDesc = reinterpret_cast<SamplerDesc*>(regs.ebx);
                const auto nDeviceMax = *reinterpret_cast<uint32_t*>(regs.esp + 0x20);

                RecordSampler(pDesc, nDeviceMax);
                ApplyAnisotropy(pDesc, nDeviceMax);
            });

            // The sampler state manager, taken from the one place its pointer is loaded with the
            // vertex sampler base right behind it. That base, 101h, is what makes the site unique.
            auto managerPattern = dunia_pattern("8B 41 0C 85 C0 74 20 8B 4C 24 08 0F B7 11 8B 0D ? ? ? ? 81 C2 01 01 00 00");
            if (!managerPattern.empty())
                ppSamplerManager = *managerPattern.get_first<void**>(16);

            static auto AnisotropicFilteringCB = []()
            {
                nAnisotropicFiltering = static_cast<uint32_t>(JackalFixSettings.GetInt(PREF_ANISOTROPICFILTERING));
            };

            AnisotropicFilteringCB();

            // The samplers are parsed once, but they are programmed from the descriptor on every
            // bind, so a change lands on the next frame rather than the next launch.
            JackalFix::onIniFileChange() += []()
            {
                AnisotropicFilteringCB();
                ReapplyAnisotropy();
            };
        };
    }
} AnisotropicFiltering;
