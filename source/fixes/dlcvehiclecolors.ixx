/* Based on Boggalog's Far Cry 2 Patched */

module;

#include <common.hxx>

export module dlcvehiclecolors;

import common;
import dunia;

// CRC-32 of the normalised material paths under graphics\_materials\.
struct MaterialRedirect
{
    uint32_t nSingleplayer;
    uint32_t nMultiplayer;
};

static constexpr MaterialRedirect Redirects[] =
{
    { 0x7B245DBF, 0x2D997F25 }, // sdore2-m-2008081267040340 -> ...101549108296  UNIMOG_PAINT01
    { 0x187972D8, 0x7262759B }, // sdore2-m-2008081958233898 -> ...101549131546  UNIMOG_PAINT02
    { 0x8295E0C5, 0xF24AAAA9 }, // sdore2-m-2008100649183233 -> ...101652084625  QUAD_BODY_PLASTIC
};

// Offsets into the function: the two call opcodes and the pushed class descriptor.
static constexpr ptrdiff_t nFindCallOpcode = 0x09;
static constexpr ptrdiff_t nCreateCallOpcode = 0x29;
static constexpr ptrdiff_t nClassDescriptorOperand = 0x22;
// The descriptor moved in the GOG build.
static uintptr_t MaterialClassDescriptorRva() { return ByBuild<uintptr_t>(0x1607AE0, 0x1556ED0); }

// Prologues of the two resolved targets, checked before hooking.
//   Find:   PUSH ECX / MOV EAX,[ESP+8] / PUSH ESI / MOV ESI,ECX
//   Create: SUB ESP,0x10 / PUSH EBX / MOV EBX,[ESP+0x18]
static constexpr uint8_t FindPrologue[] = { 0x51, 0x8B, 0x44, 0x24, 0x08, 0x56, 0x8B, 0xF1 };
static constexpr uint8_t CreatePrologue[] = { 0x83, 0xEC, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x18 };

// Both __thiscall: Find one stack argument, Create two.
static SafetyHookInline FindContainerResourceHook{};
static SafetyHookInline CreateContainerResourceHook{};

// The caller's id points into entity or mesh data, so it is read only.
static const uint32_t* Redirect(const uint32_t* pId, uint32_t& nStorage)
{
    if (pId == nullptr)
        return pId;

    for (const auto& redirect : Redirects)
    {
        if (*pId != redirect.nSingleplayer)
            continue;

        nStorage = redirect.nMultiplayer;
        return &nStorage;
    }

    return pId;
}

static void* __fastcall FindContainerResource(void* pContainer, void* pEdx, const uint32_t* pId)
{
    uint32_t nRedirected = 0;
    return FindContainerResourceHook.fastcall<void*>(pContainer, pEdx, Redirect(pId, nRedirected));
}

static void* __fastcall CreateContainerResource(void* pContainer, void* pEdx, const uint32_t* pId, void* pClassDescriptor)
{
    uint32_t nRedirected = 0;
    return CreateContainerResourceHook.fastcall<void*>(pContainer, pEdx, Redirect(pId, nRedirected), pClassDescriptor);
}

// Decodes an E8 rel32 at pCall into its target.
static uint8_t* ResolveCall(uint8_t* pCall)
{
    return pCall + 5 + *reinterpret_cast<int32_t*>(pCall + 1);
}

static bool StartsWith(const uint8_t* pCode, const uint8_t* pBytes, size_t nCount)
{
    for (size_t i = 0; i < nCount; ++i)
    {
        if (pCode[i] != pBytes[i])
            return false;
    }

    return true;
}

class DLCVehicleColors
{
public:
    DLCVehicleColors()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // CResourcePtr<T>::GetOrCreate; twenty instantiations share these bytes, hence the
            // class descriptor check below.
            auto pattern = dunia_pattern("56 57 8B 7C 24 10 57 8B F1 E8 ? ? ? ? 85 C0 75 1C 39 05 ? ? ? ? 75 07 33 C9 E8 ? ? ? ? 68 ? ? ? ? 57 8B CE E8 ? ? ? ? 85 C0 8B 4C 24 0C 5F 89 01 C6 41 04 00 5E 74 04 83 40 04 01 8B C1 C2 08 00");
            if (pattern.empty())
                return;

            const auto nMaterialClassDescriptor = reinterpret_cast<uintptr_t>(hDunia) + MaterialClassDescriptorRva();

            for (size_t i = 0; i < pattern.size(); ++i)
            {
                auto* pFunction = pattern.get(i).get<uint8_t>(0);
                if (*reinterpret_cast<uintptr_t*>(pFunction + nClassDescriptorOperand) != nMaterialClassDescriptor)
                    continue;

                auto* pFind = ResolveCall(pFunction + nFindCallOpcode);
                auto* pCreate = ResolveCall(pFunction + nCreateCallOpcode);

                if (!StartsWith(pFind, FindPrologue, sizeof(FindPrologue))
                    || !StartsWith(pCreate, CreatePrologue, sizeof(CreatePrologue)))
                    return;

                // Materials resolve once each, during load, so no ini watch.
                FindContainerResourceHook = safetyhook::create_inline(pFind, FindContainerResource);
                CreateContainerResourceHook = safetyhook::create_inline(pCreate, CreateContainerResource);
                return;
            }
        };
    }
} DLCVehicleColors;
