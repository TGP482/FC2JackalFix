/*
  Credit to https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer for identifying the feature.
  The implementation here is not his: his No Blinking Items option writes 0x2E over the first byte
  of the strings "Mesh_Highlight" (0x10E49D08), "archBlink" (0x10E115B8) and
  "gadgets.ObjectiveIcons.SaveDisk" (0x10E933B3), so the three name lookups miss. That works, but it
  corrupts shared .rdata for the life of the process, cannot be undone, and in the highlight case
  only empties the effect handle - the extra draw call still goes out, now with the base shader
  flags, which is overdraw the renderer never asked for.

  This module goes at the code instead: the effect lookup and the branch that emits the highlight
  draw, and the paths by which a marker is handed a blinking icon archetype. Nothing shared is
  modified and every piece is reversible.
*/

module;

#include <common.hxx>

export module blinkingitems;

import common;
import dunia;
import settings;

// Read once at Dunia init and refreshed on ini change. The hooks below stay installed either way
// and consult this, because a mid-hook is not worth uninstalling for a boolean.
static bool bNoBlinkingItems = false;

// A marker carries ten archetype slots at this+0x10, 0x1C bytes apart, registered by name in
// FUN_1004EFC0: 0 archMapEnabled, 1 archMapAvailable, 2 archMapDir, 3 archCompass, 4 archCompassDir,
// 5 archBlink, 6 archBlinkCompass, 7 archCompassVehicle, 8 archCompassDirVehicle,
// 9 archBlinkCompassVehicle. The three blink slots hold the alternate icon a marker flips to; leave
// them unset and the marker just draws its steady icon.
static constexpr uint32_t BLINK_SLOT_MAP = 5;
static constexpr uint32_t BLINK_SLOT_COMPASS = 6;
static constexpr uint32_t BLINK_SLOT_COMPASS_VEHICLE = 9;

static bool IsBlinkSlot(uintptr_t slot)
{
    return slot == BLINK_SLOT_MAP || slot == BLINK_SLOT_COMPASS || slot == BLINK_SLOT_COMPASS_VEHICLE;
}

// The property descriptor keeps its name pointer at +4, right after the vtable, so the data-driven
// path can be filtered by name rather than by slot index. That matters: the accessor it goes
// through is shared by every archetype-reference property in the engine, not just the marker's.
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

            // Half one, the pulsing outline on anything you can pick up or use - dropped weapons,
            // ammo and health boxes, diamond cases, beds. Two interventions, because the branch
            // gate below turned out to cover fewer objects in practice than the effect itself does.
            //
            // The mesh renderer's constructor (FUN_103C9830) resolves the highlight effect and its
            // special-pickup permutation into two of its own fields:
            //
            //   MOV  ECX, [effect manager]
            //   PUSH "Mesh_Highlight"
            //   CALL FUN_10433500                ; -> EAX = effect id (1-based), 0 = not found
            //   ...
            //   MOV  [ESI+0x6C], EDX             ; high half
            //   MOV  [ESI+0x68], EAX             ; Mesh_Highlight
            //   CALL FUN_10433DB0                ; SPECIALPICKUP permutation within that effect
            //   MOV  [ESI+0x50], EAX
            //   MOV  [ESI+0x54], EDX
            //
            // Zeroing the id the lookup returns leaves all four fields at 0 - and FUN_10433DB0
            // returns 0 for effect 0, so the permutation follows for free. Every consumer of the
            // highlight effect, wherever it lives, then has nothing to draw with. That is the same
            // end state FoxAhead reaches by corrupting the name, reached from the register instead,
            // so no shared string is touched and the game's effect table is left consistent.
            //
            // Read at renderer construction, which happens once, so this half needs a restart.
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

            // Second, the branch that emits the highlight draw at all. With the effect zeroed above
            // the clone would still be built and submitted, just with nothing to render it; the gate
            // skips that work outright:
            //
            //   TEST byte ptr [EDX+0x9C], 4      ; entity flags: "this one is highlighted"
            //   JZ   skip
            //   <clone the draw command already built for this mesh, re-submit with the highlight
            //    effect, OR'd with SPECIALPICKUP when the material's SpecialPickup bool is set>
            //
            // Clearing the immediate turns TEST into a comparison against zero, which always sets
            // ZF, so the JZ always takes. This half does re-read the ini live.
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

            // Half two, the save disk icon on the map and GPS.
            //
            // Its marker is built in code rather than from data (FUN_10698DA0), and it assigns its
            // blink pair through CMarker::SetArchetypeByName(slot, name) with slots 5 and 6. That
            // function already refuses out-of-range slots:
            //
            //   MOV EAX, [ESP+4]     ; slot
            //   CMP EAX, 9
            //   PUSH ESI
            //   MOV ESI, ECX
            //   JA  done             ; nothing assigned, nothing dirtied
            //
            // Pushing a blink slot past 9 hands the request to the engine's own reject path, which
            // leaves the slot empty and skips the Dirty/DirtyTime writes at this+0x175 / +0x178 as
            // well - exactly the state of a marker that was never given a blink icon. The hook sits
            // on the CMP, by which point the load above has already put the slot in EAX, and the
            // CMP itself runs from the trampoline afterwards against the value we leave there.
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

            // Half three, every other blinking marker - objectives, safe houses, the compass ones -
            // which get their blink archetype from the data files through the property system
            // rather than in code.
            //
            // The archetype-slot property descriptor (vtable 0x10E99A04) has two accessors in its
            // first two vtable slots, byte-identical apart from which visitor method they dispatch
            // to - +0x68 in one, +0xDC in the other. Both read the index from this+0x10 and the
            // group offset from this+0xC, compute object + offset + index*0x1C, and hand that
            // address to the visitor. Which of the two carries a value in and which carries it out
            // is not settled, so both are suppressed for the blink names. That is symmetric either
            // way: the slot keeps the empty state the marker was constructed with.
            //
            // The filter is the descriptor's name pointer at +4, not the index, because these two
            // functions serve every archetype-reference property in the engine and an index of 5 is
            // meaningless without knowing whose table it indexes. Redirecting EIP to the function's
            // own RET 8 is safe from the entry hook: nothing has been pushed yet, so the return
            // address and both stack arguments are exactly where that RET expects them.
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

                // Visitor slot +0xDC. The larger displacement makes the dispatch three bytes longer,
                // so RET 8 lands at +0x2F.
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
