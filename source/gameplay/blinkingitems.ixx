/* Based on FoxAhead's Far Cry 2 Multi Fixer: https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer */

module;

#include <common.hxx>

export module blinkingitems;

import common;
import dunia;
import settings;

// Installed either way and consulted by the hooks, so the ini is live.
static bool bNoBlinkingItems = false;

// Marker archetype slots at this+0x10, stride 0x1C: 5 archBlink, 6 archBlinkCompass,
// 9 archBlinkCompassVehicle. Left unset, the marker draws its steady icon only.
static constexpr uint32_t nBlinkSlotMap = 5;
static constexpr uint32_t nBlinkSlotCompass = 6;
static constexpr uint32_t nBlinkSlotCompassVehicle = 9;

// Past the highest slot SetArchetypeByName accepts, so its CMP/JA bails.
static constexpr uint32_t nOutOfRangeSlot = 10;

// Property descriptor name pointer.
static constexpr uintptr_t nPropertyName = 0x04;

// Into each visitor dispatch pattern: its RET 8.
static constexpr ptrdiff_t nVisit68Ret = 0x2C;
static constexpr ptrdiff_t nVisitDCRet = 0x2F;

static uintptr_t nVisit68Return = 0;
static uintptr_t nVisitDCReturn = 0;

static SafetyHookMid MarkerSetArchetypeHook{};
static SafetyHookMid ArchetypeVisit68Hook{};
static SafetyHookMid ArchetypeVisitDCHook{};

static bool IsBlinkSlot(uintptr_t slot)
{
    return slot == nBlinkSlotMap || slot == nBlinkSlotCompass || slot == nBlinkSlotCompassVehicle;
}

// The accessor serves every archetype reference property, so the data driven path is filtered by
// name.
static bool IsBlinkArchetypeProperty(const char* name)
{
    if (name == nullptr)
        return false;

    return _stricmp(name, "archBlink") == 0
        || _stricmp(name, "archBlinkCompass") == 0
        || _stricmp(name, "archBlinkCompassVehicle") == 0;
}

// On the CMP, after the slot is loaded into EAX.
static void MarkerSetArchetype(SafetyHookContext& regs)
{
    if (bNoBlinkingItems && IsBlinkSlot(regs.eax))
        regs.eax = nOutOfRangeSlot;
}

// At the function entry nothing is pushed yet, so jumping straight to its own RET 8 is safe.
static void SkipBlinkVisit(SafetyHookContext& regs, uintptr_t nReturn)
{
    if (!bNoBlinkingItems)
        return;

    if (!IsBlinkArchetypeProperty(*reinterpret_cast<const char**>(regs.ecx + nPropertyName)))
        return;

    regs.eip = nReturn;
}

static void ArchetypeVisit68(SafetyHookContext& regs)
{
    SkipBlinkVisit(regs, nVisit68Return);
}

static void ArchetypeVisitDC(SafetyHookContext& regs)
{
    SkipBlinkVisit(regs, nVisitDCReturn);
}

// Highlight gate: TEST [reg+0x9C],4 with the immediate cleared, so the JZ always skips. Compiled
// twice: FUN_103C6260 plain pass, FUN_103C7750 instanced (runs under five).
static void InstallHighlightPatches()
{
    // Plain pass: item in EDX, effect spilled at +40/+44.
    if (auto* pGate = dunia_find("8B 44 24 40 8B 4C 24 44 8B 54 24 18 89 43 10 89 4B 14 F6 82 9C 00 00 00 04 0F 84", 24))
    {
        static raw_mem fnItemHighlight(pGate, { 0x00 });
        BindPatch(fnItemHighlight, PREF_NOBLINKINGITEMS);
    }

    // Instanced pass: item in ECX, effect spilled at +30/+34.
    if (auto* pGate = dunia_find("8B 54 24 30 8B 44 24 34 8B 4C 24 18 89 53 10 89 43 14 F6 81 9C 00 00 00 04 0F 84", 24))
    {
        static raw_mem fnItemHighlightInstanced(pGate, { 0x00 });
        BindPatch(fnItemHighlightInstanced, PREF_NOBLINKINGITEMS);
    }
}

// Data driven blink archetypes: the slot descriptor dispatches through visitor +0x68 or +0xDC, and
// which one is not settled, so both are suppressed.
static void InstallVisitorHooks()
{
    // Visitor +0x68: three byte dispatch.
    auto pattern = dunia_pattern("8B C1 8B 50 10 8B 4C 24 08 56 8B 31 57 8D 3C D5 00 00 00 00 2B FA 8B 50 0C 8D 14 BA 03 54 24 0C 83 C0 04 52 50 8B 46 68 FF D0 5F 5E C2 08 00");
    if (!pattern.empty())
    {
        nVisit68Return = reinterpret_cast<uintptr_t>(pattern.get_first(nVisit68Ret));
        ArchetypeVisit68Hook = safetyhook::create_mid(pattern.get_first(0), ArchetypeVisit68);
    }

    // Visitor +0xDC: longer dispatch.
    pattern = dunia_pattern("8B C1 8B 50 10 8B 4C 24 08 56 8B 31 57 8D 3C D5 00 00 00 00 2B FA 8B 50 0C 8D 14 BA 03 54 24 0C 83 C0 04 52 50 8B 86 DC 00 00 00 FF D0 5F 5E C2 08 00");
    if (!pattern.empty())
    {
        nVisitDCReturn = reinterpret_cast<uintptr_t>(pattern.get_first(nVisitDCRet));
        ArchetypeVisitDCHook = safetyhook::create_mid(pattern.get_first(0), ArchetypeVisitDC);
    }
}

class BlinkingItems
{
public:
    BlinkingItems()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            BindBool(bNoBlinkingItems, PREF_NOBLINKINGITEMS);

            InstallHighlightPatches();

            // Save disk icon: CMarker::SetArchetypeByName already rejects slots above 9 (CMP EAX,9 / JA),
            // so blink slots are pushed past 9.
            if (auto* p = dunia_find("8B 44 24 04 83 F8 09 56 8B F1 77 38 8B 4C 24 0C 6A FF", 4))
                MarkerSetArchetypeHook = safetyhook::create_mid(p, MarkerSetArchetype);

            InstallVisitorHooks();
        };
    }
} BlinkingItems;
