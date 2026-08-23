module;

#include <common.hxx>

export module dunia;

import common;

// FarCry2.exe is a stub calling Dunia.dll's RunGame, so patterns scan Dunia's image.
export HMODULE hDunia = nullptr;

export hook::pattern dunia_pattern(std::string_view bytes)
{
    return hook::module_pattern(hDunia, bytes);
}

// Null when the pattern misses.
export void* dunia_find(std::string_view bytes, ptrdiff_t offset = 0)
{
    auto pattern = dunia_pattern(bytes);
    return pattern.empty() ? nullptr : pattern.get_first(offset);
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

export void InitDunia()
{
    hDunia = GetModuleHandleW(L"Dunia.dll");
    if (!hDunia)
        return;

    JackalFix::onDuniaInitEvent().executeAll();
}
