/*
  Two hang glider bug fixes from An Almost Complete Guide to Far Cry 2 Modding, in code:
  "Hang gliders falling out of the sky when shot" and "Hang gliders bouncing on water".

  The guide's versions are data edits to every Air.Paraglider* prototype in
  entitylibrarypatchoverride.fcb - three floats, spread over two components:

    ParagliderParams.fMass                     300.0 -> 2420.0   (shot out of the sky)
    CVehicleParagliderPhysComponent
        .fDiscardedMass                         75.0 ->  825.0   (bouncing on water)
    CVehicle.fUnderWaterMaxDepth                -1.0 ->    1.5   (bouncing on water)

  All three need a repacked patch.dat, which collides with every other mod that ships one. All
  three are engine fields at fixed offsets, so all three can be written at the moment the engine
  reaches for them instead. Nothing below is toggleable, because none of it is a preference.

  ---------------------------------------------------------------------------------------------
  fMass - CVehicleParagliderPhysComponent + 0x1B0

  FUN_10492430 registers the ParagliderParams schema, whose owning class is
  CPhysParagliderVehicleEntityCreateParams: fMass at +0x90, then fMaxRollAngle 0xC0 through
  fDownSpeed 0xF8. FUN_10073ED0 embeds that struct in the vehicle component at +0x120, so the
  loaded fMass lands at component + 0x1B0.

  It is consumed in FUN_104A15B0, the rigid body builder, twice - once as mass over shape volume
  to seed the inertia tensor, once straight into hkpRigidBody::setMass. That function is not a
  hook site: it is referenced from five physics-entity vtables, and +0x90 is a base-class field
  the shared operator= (FUN_10070EE0) copies for other vehicles too.

  The last chokepoint that is provably paraglider-only is FUN_104931D0, reached from exactly one
  caller - FUN_1006D6B0, slot +0x128 of the CVehicleParagliderPhysComponent vtable 0x10E13498:

    CreateParagliderPhysEntity(ParagliderParams* params, PhysWorld* world)   __cdecl
      new CPhysParagliderVehicleEntity(0x2B0)     ; ctor FUN_104A80B0, vtable 0x10E67108
      new handle wrapper(0x60)
      MOV EBX, [ESP+0x18]                         ; params
      ...
      entity->vtbl[0xD8](params, wrapper)         ; Init -> base init -> FUN_104A15B0

  Every paraglider gets its Havok body through here and nothing else does, so writing 2420 into
  params->fMass on the way in is the data edit, applied at the one moment before any consumer
  runs: before setMass, before the inertia tensor, and before Init copies the params into the
  entity's own slot at +0x190. The hook sits on MOV ESI,EAX / ADD ESP,8, five bytes exactly, by
  which point EBX already holds params and EAX still holds the allocation the MOV wants.

  ---------------------------------------------------------------------------------------------
  fDiscardedMass - CVehicleParagliderPhysComponent + 0x220

  Registered by FUN_10073ED0 and seeded to 75.0 by the component's constructor at 0x1006D678.
  The name string is referenced from that one schema, so the field is the paraglider's alone.

  Its only reader is FUN_1006A230, slot +0x11C of the component vtable - a paraglider override of
  a virtual the base CVehiclePhysComponent leaves as a bare RET. Its one caller is the "get out
  vehicle" handler FUN_100E5B40, which fires once the player has let go and walked two metres
  away:

    CMP  [ESI+0x64], 0        ; no body -> bail
    ...  body->GetType() == 7
    MOV  ECX, [[ESI+0x60]]    ; hkpRigidBody
    FLD  [ESI+0x220]          ; fDiscardedMass
    CALL hkpRigidBody::setMass

  So this is the mass an abandoned canopy is given, and 75kg is what lets it skitter across water.
  The hook covers the six-byte FLD and writes the field first, so the FLD reads 825 from the
  trampoline.

  ---------------------------------------------------------------------------------------------
  fUnderWaterMaxDepth - CVehicle + 0x114

  Registered by FUN_100DEA10 and seeded to -1.0 by the CVehicle constructor at 0x100E11BE. Unlike
  the other two this field belongs to every vehicle in the game - the big truck ships 1.95 - so it
  cannot be rewritten blind.

  Its only reader in the whole image is FUN_100DA270, CVehicle::IsSubmerged:

    COMISS XMM0, [EBX+0x114]  ; 0.0 vs fUnderWaterMaxDepth
    JNC    not_submerged      ; <= 0 short-circuits to false
    ... entity->GetPosition(&pos)
    ... WaterMgr::GetHeightAt(pos.x, pos.y)
    FLD    [ESP+0x14]         ; pos.z
    FADD   [EBX+0x114]
    FCOMIP                    ; waterZ > z + depth -> submerged

  Worth being precise about, because it is the opposite of what the symptom suggests: -1 does not
  make the test always trip, it disables it. A stock paraglider is never recognised as submerged,
  which is why it is never retired and instead sits on the surface being pushed around by it. 1.5
  arms the check. The three callers - FUN_100DB6E0, FUN_100DC930, FUN_100E3450 - are the vehicle
  entry and usability gates, so this runs for a nearby vehicle rather than every vehicle
  every frame.

  The paraglider is identified by sName, the vehicle's own name hash, which the schema puts at
  CVehicle + 0 and the constructor seeds to 0xFFFFFFFF. Every Air.Paraglider* prototype carries
  the same "paraglider" hash and no other vehicle in the library does. iAnimVehicleType at +4 is
  2 for the paraglider, but it is an animation set rather than an identity - the big truck's is 8,
  not the 0 a vehicle-type enum would give it - so it is not used here.
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

// The guide's values, all three arrived at by its author through testing. Heavier than 2420 and
// gunfire stops registering at all; lighter and the glider still flips. 825 is buoyant enough to
// settle and still take the player's weight, and 1.5 is the depth at which a settled glider
// counts as submerged.
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
            // Shot out of the sky. Matched from the entry of the paraglider physics factory to
            // the second allocation, stopping before the first absolute address in the function
            // (the vtable store at +0x47). Both call displacements are wildcarded.
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

            // Bouncing on water, half one: the mass a discarded canopy is handed. The whole of
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

            // Bouncing on water, half two: arming the submersion test. CVehicle::IsSubmerged from
            // its entry through the entity fetch, stopping before the first call displacement.
            // The hook covers the seven-byte COMISS, which then reads the rewritten field.
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
