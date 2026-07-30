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

export void InitDunia()
{
    hDunia = GetModuleHandleW(L"Dunia.dll");
    if (!hDunia)
        return;

    JackalFix::onDuniaInitEvent().executeAll();
}
