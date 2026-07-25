module;

#include <common.hxx>

export module dunia;

import common;

// FarCry2.exe is a 28KB stub that does nothing but call Dunia.dll's RunGame export,
// so every pattern has to be scanned against Dunia's image rather than the main module.
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
