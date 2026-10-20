module;

#include <common.hxx>
#include <cwctype>

export module keylabels;

import common;
import dunia;

// DirectInput keys are physical positions already, but the Controls page and prompts name them
// from the US string table. Letters, digits and symbols are renamed from the player's layout.

// US set 1 scan codes, by the single character Dunia names each key with.
struct KeyRow
{
    std::string_view sNames;
    UINT nFirstScanCode;
};

static constexpr KeyRow KeyRows[]
{
    { "1234567890-=", 0x02 },
    { "qwertyuiop[]", 0x10 },
    { "asdfghjkl;'`", 0x1E },
    { "\\zxcvbnm,./", 0x2B },
};

static constexpr std::string_view szIsoKey = "oem_102";
static constexpr UINT nIsoKeyScanCode = 0x56;

// Into the label lookup, after wcslen: ESI the label, EAX its length, the key name in the frame.
static constexpr ptrdiff_t nLabelReady = 0xB4;
static constexpr ptrdiff_t nKeyNameArg = 0x54;

static wchar_t szLabel[2]{};

static SafetyHookMid KeyLabelHook{};

static UINT ScanCodeOf(std::string_view sName)
{
    if (sName == szIsoKey)
        return nIsoKeyScanCode;

    if (sName.size() != 1)
        return 0;

    for (const auto& row : KeyRows)
    {
        const auto nIndex = row.sNames.find(sName[0]);
        if (nIndex != std::string_view::npos)
            return row.nFirstScanCode + static_cast<UINT>(nIndex);
    }

    return 0;
}

// The unshifted character the active layout prints there, ASCII only; 0 keeps the US name.
static wchar_t LayoutCharacter(UINT nScanCode)
{
    auto hLayout = GetKeyboardLayout(0);
    auto nVirtualKey = MapVirtualKeyExW(nScanCode, MAPVK_VSC_TO_VK, hLayout);

    // High bit marks a dead key; its character is still the one printed on the key.
    auto nCharacter = MapVirtualKeyExW(nVirtualKey, MAPVK_VK_TO_CHAR, hLayout) & 0x7FFF;
    if (nCharacter <= L' ' || nCharacter > L'~')
        return 0;

    return static_cast<wchar_t>(std::towupper(static_cast<wint_t>(nCharacter)));
}

static void RenameKey(SafetyHookContext& regs)
{
    auto pszName = *reinterpret_cast<const char**>(regs.esp + nKeyNameArg);
    if (pszName == nullptr)
        return;

    auto nScanCode = ScanCodeOf(pszName);
    if (nScanCode == 0)
        return;

    auto chLabel = LayoutCharacter(nScanCode);
    if (chLabel == 0)
        return;

    szLabel[0] = chLabel;
    regs.esi = reinterpret_cast<uintptr_t>(szLabel);
    regs.eax = 1;
}

class KeyLabels
{
public:
    KeyLabels()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // The keyboard entry of the input display name table.
            if (auto* p = dunia_find("83 EC 3C 53 56 57 8D 44 24 0F 50 8D 4C 24 34 33 DB BF 0F 00 00 00 51", nLabelReady))
                KeyLabelHook = safetyhook::create_mid(p, RenameKey);
        };
    }
} KeyLabels;
