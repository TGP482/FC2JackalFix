/*
  Two hang glider bug fixes from An Almost Complete Guide to Far Cry 2 Modding: gliders falling
  out of the sky when shot, and gliders bouncing on water. The guide edits three floats in every
  Air.Paraglider* prototype in entitylibrarypatchoverride.fcb:

    ParagliderParams.fMass                     300.0 -> 2420.0   (shot out of the sky)
    CVehicleParagliderPhysComponent
        .fDiscardedMass                         75.0 ->  825.0   (bouncing on water)
    CVehicle.fUnderWaterMaxDepth                -1.0 ->    1.5   (bouncing on water)

  That needs a repacked patch.dat, which collides with every other mod that ships one. All three
  are engine fields at fixed offsets, so they are written as the engine reaches for them instead.

  fMass, at CVehicleParagliderPhysComponent + 0x1B0. FUN_10492430 registers the ParagliderParams
  schema on CPhysParagliderVehicleEntityCreateParams with fMass at +0x90, and FUN_10073ED0 embeds
  that struct in the component at +0x120. Its consumer FUN_104A15B0 (inertia tensor, then
  hkpRigidBody::setMass) is no use as a hook site: five physics-entity vtables reference it, and
  +0x90 is a base-class field the shared operator= FUN_10070EE0 copies for other vehicles. The
  paraglider-only chokepoint is the factory FUN_104931D0, CreateParagliderPhysEntity(params,
  world) __cdecl, reached only from FUN_1006D6B0 at slot +0x128 of vtable 0x10E13498. Writing
  params->fMass there beats setMass, the inertia tensor and Init's copy into the entity's own
  slot at +0x190. The hook sits on MOV ESI,EAX / ADD ESP,8, five bytes exactly, where EBX already
  holds params and EAX still holds the allocation the MOV wants.

  fDiscardedMass, at CVehicleParagliderPhysComponent + 0x220. Registered by FUN_10073ED0, seeded to
  75.0 by the component constructor at 0x1006D678, and named from that one schema. Only reader is
  FUN_1006A230, slot +0x11C of the component vtable, called by the get-out-vehicle handler
  FUN_100E5B40 once the player has walked two metres away: FLD [ESI+0x220] into
  hkpRigidBody::setMass. 75kg is what lets an abandoned canopy skitter across water. The hook
  covers the six-byte FLD and writes the field first, so the FLD reads 825 from the trampoline.

  fUnderWaterMaxDepth, at CVehicle + 0x114. Registered by FUN_100DEA10, seeded to -1.0 by the
  CVehicle constructor at 0x100E11BE. Belongs to every vehicle in the game (the big truck ships
  1.95), so it cannot be rewritten blind. Only reader is FUN_100DA270, CVehicle::IsSubmerged:

    COMISS XMM0, [EBX+0x114]  ; 0.0 vs fUnderWaterMaxDepth
    JNC    not_submerged      ; <= 0 short-circuits to false
    FLD    [ESP+0x14]         ; pos.z
    FADD   [EBX+0x114]
    FCOMIP                    ; waterZ > z + depth -> submerged

  -1 disables the test rather than always tripping it, so a stock paraglider is never recognised
  as submerged, never retired, and sits on the surface being pushed around by it. 1.5 arms the
  check. Callers FUN_100DB6E0, FUN_100DC930 and FUN_100E3450 are the vehicle entry and usability
  gates, so this runs for a nearby vehicle rather than every vehicle every frame.

  Identified by sName at CVehicle + 0, the vehicle's own name hash: every Air.Paraglider*
  prototype carries the same one and no other vehicle does. iAnimVehicleType at +4 is 2 for the
  paraglider but is an animation set rather than an identity (the big truck's is 8).
*/

module;

#include <common.hxx>

export module glider;

import common;
import dunia;

// ParagliderParams, relative to the struct the physics factory is handed.
static constexpr uint32_t nParagliderParamsMass = 0x90;

// CVehicleParagliderPhysComponent.
static constexpr uint32_t nParagliderPhysDiscardedMass = 0x220;

// CVehicle. sName is the hash of the vehicle's own name string, "paraglider".
static constexpr uint32_t nVehicleName = 0x00;
static constexpr uint32_t nVehicleUnderWaterMaxDepth = 0x114;
static constexpr uint32_t nVehicleNameParaglider = 0x7B2D589C;

// The guide's values, arrived at by its author through testing. Heavier than 2420 and gunfire
// stops registering at all; lighter and the glider still flips.
static constexpr float fParagliderMass = 2420.0f;
static constexpr float fParagliderDiscardedMass = 825.0f;
static constexpr float fParagliderUnderWaterMaxDepth = 1.5f;

class Glider
{
public:
    Glider()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Shot out of the sky. Factory entry to the second allocation, stopping before the
            // first absolute address (the vtable store at +0x47). Call displacements wildcarded.
            {
                auto pattern = dunia_pattern("53 56 57 6A 00 68 B0 02 00 00 E8 ? ? ? ? 83 C4 08 85 C0 74 0B 8B C8 E8 ? ? ? ? 8B F8 EB 02 33 FF 6A 00 6A 60");
                if (!pattern.empty())
                {
                    static auto ParagliderMassHook = safetyhook::create_mid(pattern.get_first(0x30), [](SafetyHookContext& regs)
                    {
                        *reinterpret_cast<float*>(regs.ebx + nParagliderParamsMass) = fParagliderMass;
                    });
                }
            }

            // Bouncing on water, half one. The whole of
            // CVehicleParagliderPhysComponent::OnDiscarded, ending on the FLD the hook covers.
            {
                auto pattern = dunia_pattern("56 8B F1 83 7E 64 00 74 29 8B 46 60 8B 08 8B 11 8B 42 08 FF D0 83 F8 07 75 18 8B 4E 60 8B 09 85 C9 74 0F D9 86 20 02 00 00 51 D9 1C 24");
                if (!pattern.empty())
                {
                    static auto ParagliderDiscardHook = safetyhook::create_mid(pattern.get_first(0x23), [](SafetyHookContext& regs)
                    {
                        *reinterpret_cast<float*>(regs.esi + nParagliderPhysDiscardedMass) = fParagliderDiscardedMass;
                    });
                }
            }

            // Bouncing on water, half two. CVehicle::IsSubmerged from its entry through the
            // entity fetch, stopping before the first call displacement. The hook covers the
            // seven-byte COMISS, which then reads the rewritten field.
            {
                auto pattern = dunia_pattern("0F 57 C0 83 EC 0C 53 8B D9 0F 2F 83 14 01 00 00 73 63 8B 43 08 56 8B 70 08 83 46 08 01 57 8B 7E 0C 8B CF");
                if (!pattern.empty())
                {
                    static auto VehicleSubmergedHook = safetyhook::create_mid(pattern.get_first(9), [](SafetyHookContext& regs)
                    {
                        if (*reinterpret_cast<uint32_t*>(regs.ebx + nVehicleName) != nVehicleNameParaglider)
                            return;

                        *reinterpret_cast<float*>(regs.ebx + nVehicleUnderWaterMaxDepth) = fParagliderUnderWaterMaxDepth;
                    });
                }
            }
        };
    }
} Glider;
