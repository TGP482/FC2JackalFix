module;

#include <common.hxx>

export module dunia;

import common;

// FarCry2.exe is a 28KB stub that only calls Dunia.dll's RunGame export, so patterns scan
// Dunia's image instead of the main module.
export HMODULE hDunia = nullptr;

export hook::pattern dunia_pattern(std::string_view bytes)
{
    return hook::module_pattern(hDunia, bytes);
}

// The whole "match, test for empty, take the first hit at an offset" dance in one call. Null when
// the pattern does not resolve, so a module reads as `if (auto* p = dunia_find(...))`.
export void* dunia_find(std::string_view bytes, ptrdiff_t offset = 0)
{
    auto pattern = dunia_pattern(bytes);
    return pattern.empty() ? nullptr : pattern.get_first(offset);
}

export void InitDunia()
{
    hDunia = GetModuleHandleW(L"Dunia.dll");
    if (!hDunia)
        return;

    JackalFix::onDuniaInitEvent().executeAll();
}
