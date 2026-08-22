/*
  Two hang glider bug fixes from An Almost Complete Guide to Far Cry 2 Modding: gliders falling
  out of the sky when shot, and gliders bouncing on water. The guide edits three floats in every
  Air.Paraglider* prototype in entitylibrarypatchoverride.fcb
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
            if (auto* pMass = dunia_find("53 56 57 6A 00 68 B0 02 00 00 E8 ? ? ? ? 83 C4 08 85 C0 74 0B 8B C8 E8 ? ? ? ? 8B F8 EB 02 33 FF 6A 00 6A 60", 0x30))
            {
                static auto ParagliderMassHook = safetyhook::create_mid(pMass, [](SafetyHookContext& regs)
                {
                    *reinterpret_cast<float*>(regs.ebx + nParagliderParamsMass) = fParagliderMass;
                });
            }

            // Bouncing on water, half one. The whole of
            // CVehicleParagliderPhysComponent::OnDiscarded, ending on the FLD the hook covers.
            if (auto* pDiscard = dunia_find("56 8B F1 83 7E 64 00 74 29 8B 46 60 8B 08 8B 11 8B 42 08 FF D0 83 F8 07 75 18 8B 4E 60 8B 09 85 C9 74 0F D9 86 20 02 00 00 51 D9 1C 24", 0x23))
            {
                static auto ParagliderDiscardHook = safetyhook::create_mid(pDiscard, [](SafetyHookContext& regs)
                {
                    *reinterpret_cast<float*>(regs.esi + nParagliderPhysDiscardedMass) = fParagliderDiscardedMass;
                });
            }

            // Bouncing on water, half two. CVehicle::IsSubmerged from its entry through the
            // entity fetch, stopping before the first call displacement. The hook covers the
            // seven-byte COMISS, which then reads the rewritten field.
            if (auto* pSubmerged = dunia_find("0F 57 C0 83 EC 0C 53 8B D9 0F 2F 83 14 01 00 00 73 63 8B 43 08 56 8B 70 08 83 46 08 01 57 8B 7E 0C 8B CF", 9))
            {
                static auto VehicleSubmergedHook = safetyhook::create_mid(pSubmerged, [](SafetyHookContext& regs)
                {
                    if (*reinterpret_cast<uint32_t*>(regs.ebx + nVehicleName) != nVehicleNameParaglider)
                        return;

                    *reinterpret_cast<float*>(regs.ebx + nVehicleUnderWaterMaxDepth) = fParagliderUnderWaterMaxDepth;
                });
            }
        };
    }
} Glider;
