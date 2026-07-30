/*
  Credit to https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer for identifying the feature.
  His version writes 0x2E over the first byte of "Mesh_Highlight" (0x10E49D08), "archBlink"
  (0x10E115B8) and "gadgets.ObjectiveIcons.SaveDisk" (0x10E933B3) so the name lookups miss, which
  corrupts shared .rdata for the process lifetime and still emits the highlight draw call. This
  module patches the code paths instead, and every piece is reversible.
*/

module;

#include <common.hxx>

export module blinkingitems;

import common;
import dunia;
import settings;

// The hooks below stay installed either way and consult this.
static bool bNoBlinkingItems = false;

// A marker carries ten archetype slots at this+0x10, stride 0x1C, registered by name in
// FUN_1004EFC0: 0 archMapEnabled, 1 archMapAvailable, 2 archMapDir, 3 archCompass, 4 archCompassDir,
// 5 archBlink, 6 archBlinkCompass, 7 archCompassVehicle, 8 archCompassDirVehicle,
// 9 archBlinkCompassVehicle. Leave the blink slots unset and the marker draws only its steady icon.
static constexpr uint32_t BLINK_SLOT_MAP = 5;
static constexpr uint32_t BLINK_SLOT_COMPASS = 6;
static constexpr uint32_t BLINK_SLOT_COMPASS_VEHICLE = 9;

static bool IsBlinkSlot(uintptr_t slot)
{
    return slot == BLINK_SLOT_MAP || slot == BLINK_SLOT_COMPASS || slot == BLINK_SLOT_COMPASS_VEHICLE;
}

// The property descriptor keeps its name pointer at +4, after the vtable. Filter the data-driven
// path by name, since its accessor serves every archetype-reference property in the engine.
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
            bNoBlinkingItems = JackalFixSettings.GetInt(PREF_NOBLINKINGITEMS) != 0;

            JackalFix::onIniFileChange() += []()
            {
                bNoBlinkingItems = JackalFixSettings.GetInt(PREF_NOBLINKINGITEMS) != 0;
            };

            // Half one, the pulsing outline on pickups. Two interventions, since the branch gate
            // below covers fewer objects than the effect itself.
            //
            // The mesh renderer's constructor (FUN_103C9830) calls FUN_10433500("Mesh_Highlight")
            // into [ESI+0x68]/[ESI+0x6C], then FUN_10433DB0 for the SPECIALPICKUP permutation into
            // [ESI+0x50]/[ESI+0x54]. Zeroing the returned effect id (1-based, 0 = not found) leaves
            // all four at 0, since FUN_10433DB0 returns 0 for effect 0. Runs once at construction,
            // so this half needs a restart.
            {
                auto pattern = dunia_pattern("E8 ? ? ? ? 53 8B CA 68 ? ? ? ? 89 56 6C 51 8B D0 89 46 68");
                if (!pattern.empty())
                {
                    static auto MeshHighlightLookupHook = safetyhook::create_mid(pattern.get_first(5), [](SafetyHookContext& regs)
                    {
                        if (!bNoBlinkingItems)
                            return;

                        regs.eax = 0; // effect id: not found
                        regs.edx = 0;
                    });
                }
            }

            // Second, the branch that emits the highlight draw at all, which the zeroed effect
            // above does not stop:
            //
            //   TEST byte ptr [EDX+0x9C], 4      ; entity flags: highlighted
            //   JZ   skip
            //
            // Clearing the immediate makes TEST always set ZF, so the JZ always takes. This half
            // re-reads the ini live.
            {
                auto pattern = dunia_pattern("8B 44 24 40 8B 4C 24 44 8B 54 24 18 89 43 10 89 4B 14 F6 82 9C 00 00 00 04 0F 84");
                if (!pattern.empty())
                {
                    static raw_mem fnItemHighlight(pattern.get_first(24), { 0x00 }); // TEST ...,4 -> TEST ...,0

                    static auto ItemHighlightCB = []()
                    {
                        if (bNoBlinkingItems)
                            fnItemHighlight.Write();
                        else
                            fnItemHighlight.Restore();
                    };

                    ItemHighlightCB();

                    JackalFix::onIniFileChange() += []()
                    {
                        ItemHighlightCB();
                    };
                }
            }

            // Half two, the save disk icon on the map and GPS. Its marker is built in code
            // (FUN_10698DA0), assigning slots 5 and 6 through CMarker::SetArchetypeByName, which
            // already rejects slots above 9 (CMP EAX,9 / JA done). Pushing a blink slot past 9
            // takes that reject path, which also skips the Dirty/DirtyTime writes at this+0x175 /
            // +0x178. Hook is on the CMP, after the slot load, so the CMP runs from the trampoline
            // against the value left in EAX.
            {
                auto pattern = dunia_pattern("8B 44 24 04 83 F8 09 56 8B F1 77 38 8B 4C 24 0C 6A FF");
                if (!pattern.empty())
                {
                    static auto MarkerSetArchetypeHook = safetyhook::create_mid(pattern.get_first(4), [](SafetyHookContext& regs)
                    {
                        if (!bNoBlinkingItems)
                            return;

                        if (IsBlinkSlot(regs.eax))
                            regs.eax = 10; // out of range, so the CMP/JA two instructions down bails
                    });
                }
            }

            // Half three, every other blinking marker, which gets its blink archetype from the
            // data files through the property system.
            //
            // The archetype-slot property descriptor (vtable 0x10E99A04) has two accessors in its
            // first two vtable slots, identical apart from the visitor they dispatch to, +0x68 and
            // +0xDC. Both hand object + this+0xC + index*0x1C to the visitor, with the index from
            // this+0x10. Which one carries the value in and which out is unsettled, so both are
            // suppressed for the blink names; either way the slot keeps its empty state.
            //
            // Filtered on the descriptor name at +4, not the index, since both serve every
            // archetype-reference property. Redirecting EIP to the function's own RET 8 is safe
            // from the entry hook: nothing has been pushed yet.
            {
                // Visitor slot +0x68. Three-byte dispatch, so RET 8 lands at +0x2C.
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

                // Visitor slot +0xDC. Larger displacement, so the dispatch is three bytes longer
                // and RET 8 lands at +0x2F.
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
