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
static constexpr uint32_t BLINK_SLOT_MAP = 5;
static constexpr uint32_t BLINK_SLOT_COMPASS = 6;
static constexpr uint32_t BLINK_SLOT_COMPASS_VEHICLE = 9;

static bool IsBlinkSlot(uintptr_t slot)
{
    return slot == BLINK_SLOT_MAP || slot == BLINK_SLOT_COMPASS || slot == BLINK_SLOT_COMPASS_VEHICLE;
}

// Descriptor name pointer at +4; the accessor serves every archetype-reference property, so
// the data-driven path is filtered by name.
static bool IsBlinkArchetypeProperty(const char* name)
{
    return name != nullptr
        && (_stricmp(name, "archBlink") == 0
            || _stricmp(name, "archBlinkCompass") == 0
            || _stricmp(name, "archBlinkCompassVehicle") == 0);
}

class BlinkingItems
{
public:
    BlinkingItems()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            BindBool(bNoBlinkingItems, PREF_NOBLINKINGITEMS);

            // Highlight gate: TEST [reg+0x9C],4 with the immediate cleared, so the JZ always skips.
            // Compiled twice: FUN_103C6260 plain pass, FUN_103C7750 instanced (runs under five).
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

            // Save disk icon: CMarker::SetArchetypeByName already rejects slots above 9
            // (CMP EAX,9 / JA), so blink slots are pushed past 9. Hook on the CMP, after the load.
            {
                if (auto* pMarkerSetArchetype = dunia_find("8B 44 24 04 83 F8 09 56 8B F1 77 38 8B 4C 24 0C 6A FF", 4))
                {
                    static auto MarkerSetArchetypeHook = safetyhook::create_mid(pMarkerSetArchetype, [](SafetyHookContext& regs)
                    {
                        if (!bNoBlinkingItems)
                            return;

                        if (IsBlinkSlot(regs.eax))
                            regs.eax = 10; // out of range, so the CMP/JA below bails
                    });
                }
            }

            // Data-driven blink archetypes: the slot descriptor dispatches through visitor +0x68 or
            // +0xDC, direction unsettled, so both are suppressed. EIP to the function's own RET 8 is
            // safe from the entry hook: nothing pushed yet.
            {
                // Visitor +0x68: three-byte dispatch, RET 8 at +0x2C.
                auto pattern = dunia_pattern("8B C1 8B 50 10 8B 4C 24 08 56 8B 31 57 8D 3C D5 00 00 00 00 2B FA 8B 50 0C 8D 14 BA 03 54 24 0C 83 C0 04 52 50 8B 46 68 FF D0 5F 5E C2 08 00");
                if (!pattern.empty())
                {
                    static uintptr_t nVisit68Return = reinterpret_cast<uintptr_t>(pattern.get_first(0x2C));

                    static auto ArchetypeVisit68Hook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                    {
                        if (!bNoBlinkingItems)
                            return;

                        if (!IsBlinkArchetypeProperty(*reinterpret_cast<const char**>(regs.ecx + 4)))
                            return;

                        regs.eip = nVisit68Return;
                    });
                }

                // Visitor +0xDC: longer dispatch, RET 8 at +0x2F.
                pattern = dunia_pattern("8B C1 8B 50 10 8B 4C 24 08 56 8B 31 57 8D 3C D5 00 00 00 00 2B FA 8B 50 0C 8D 14 BA 03 54 24 0C 83 C0 04 52 50 8B 86 DC 00 00 00 FF D0 5F 5E C2 08 00");
                if (!pattern.empty())
                {
                    static uintptr_t nVisitDCReturn = reinterpret_cast<uintptr_t>(pattern.get_first(0x2F));

                    static auto ArchetypeVisitDCHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                    {
                        if (!bNoBlinkingItems)
                            return;

                        if (!IsBlinkArchetypeProperty(*reinterpret_cast<const char**>(regs.ecx + 4)))
                            return;

                        regs.eip = nVisitDCReturn;
                    });
                }
            }
        };
    }
} BlinkingItems;
