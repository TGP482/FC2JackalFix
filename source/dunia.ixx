module;

#include <common.hxx>

export module dunia;

import common;
import logging;

// FarCry2.exe is a stub calling Dunia.dll's RunGame, so patterns scan Dunia's image.
export HMODULE hDunia = nullptr;

// The two retail builds are separate compiles of the same 1.03 source, so most of the image is
// byte identical while a handful of functions, globals and one class layout are not. Dunia.dll's
// PE TimeDateStamp tells them apart for the cost of two dereferences and no scan.
static constexpr uint32_t nGogTimeDateStamp = 0x49FB4BF6;

// False for the Steam build, which every pattern here was written against, and for any build we
// have not seen: an unknown build is far likelier to be Steam-like than GOG-like.
export bool bDuniaGOG = false;

export bool IsGOG()
{
    return bDuniaGOG;
}

// Picks the value the running build needs, for the RVAs and struct offsets that moved.
export template <class T>
T ByBuild(T steam, T gog)
{
    return bDuniaGOG ? gog : steam;
}

export hook::pattern dunia_pattern(std::string_view bytes)
{
    return hook::module_pattern(hDunia, bytes);
}

// For the sites where one pattern cannot cover both builds.
export hook::pattern dunia_pattern(std::string_view steam, std::string_view gog)
{
    return dunia_pattern(ByBuild(steam, gog));
}

// Null when the pattern misses.
export void* dunia_find(std::string_view bytes, ptrdiff_t offset = 0)
{
    auto pattern = dunia_pattern(bytes);
    return pattern.empty() ? nullptr : pattern.get_first(offset);
}

export void* dunia_find(std::string_view steam, std::string_view gog, ptrdiff_t offset = 0)
{
    return dunia_find(ByBuild(steam, gog), offset);
}

// CPawnInputListener::Update, the engine's per-frame input pass. More than one module hooks inside
// this pattern's range, and the first hook to land rewrites bytes the pattern still asks for, so
// whoever scanned second used to find nothing. Resolved once here, on the first ask, while the
// bytes are still stock. Null when the pattern misses.
export void* dunia_input_pass()
{
    static auto* pInputPass = dunia_find("56 8B F1 74 0A 88 46 04 88 46 05 5E C2 08 00 8B 4E 20 3B C8 74 4A E8 ? ? ? ? F6 40 04 40 74 11");
    return pInputPass;
}

// Modules register their build fixups at this priority, so every build-dependent constant is
// already corrected by the time the default-priority handlers read it.
export constexpr auto nBuildFixupPriority = 10;

static uint32_t DuniaTimeDateStamp()
{
    const auto pBase = reinterpret_cast<const uint8_t*>(hDunia);
    if (!pBase)
        return 0;

    const auto pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase);
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    const auto pNt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(pBase + pDos->e_lfanew);
    return pNt->Signature == IMAGE_NT_SIGNATURE ? pNt->FileHeader.TimeDateStamp : 0;
}

export void InitDunia()
{
    hDunia = GetModuleHandleW(L"Dunia.dll");
    if (!hDunia)
        return;

    const auto nStamp = DuniaTimeDateStamp();
    bDuniaGOG = nStamp == nGogTimeDateStamp;

    LogInfo("Dunia: {} build (timestamp 0x{:08X}, base 0x{:08X})", bDuniaGOG ? "GOG" : "Steam", nStamp, reinterpret_cast<uintptr_t>(hDunia));

    JackalFix::onDuniaInitEvent().executeAll();
}
