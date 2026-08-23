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

export void InitDunia()
{
    hDunia = GetModuleHandleW(L"Dunia.dll");
    if (!hDunia)
        return;

    JackalFix::onDuniaInitEvent().executeAll();
}
