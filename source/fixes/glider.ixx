/* Based on An Almost Complete Guide to Far Cry 2 Modding */

module;

#include <common.hxx>

export module glider;

import common;
import dunia;

// ParagliderParams, relative to the struct the physics factory is handed.
static constexpr uint32_t nParagliderParamsMass = 0x90;

// CVehicleParagliderPhysComponent.
static constexpr uint32_t nParagliderPhysDiscardedMass = 0x220;

// CVehicle. sName is the hash of the vehicle's name string, "paraglider".
static constexpr uint32_t nVehicleName = 0x00;
static constexpr uint32_t nVehicleUnderWaterMaxDepth = 0x114;
static constexpr uint32_t nVehicleNameParaglider = 0x7B2D589C;

// The guide's tested values: above 2420 gunfire stops registering, below it the glider still flips.
static constexpr float fParagliderMass = 2420.0f;
static constexpr float fParagliderDiscardedMass = 825.0f;
static constexpr float fParagliderUnderWaterMaxDepth = 1.5f;

static SafetyHookMid ParagliderMassHook{};
static SafetyHookMid ParagliderDiscardHook{};
static SafetyHookMid VehicleSubmergedHook{};

// EBX is the ParagliderParams.
static void SetParagliderMass(SafetyHookContext& regs)
{
    *reinterpret_cast<float*>(regs.ebx + nParagliderParamsMass) = fParagliderMass;
}

// ESI is the physics component.
static void SetDiscardedMass(SafetyHookContext& regs)
{
    *reinterpret_cast<float*>(regs.esi + nParagliderPhysDiscardedMass) = fParagliderDiscardedMass;
}

// EBX is the vehicle. Every vehicle comes through here, so only the paraglider is rewritten.
static void SetUnderWaterMaxDepth(SafetyHookContext& regs)
{
    if (*reinterpret_cast<uint32_t*>(regs.ebx + nVehicleName) != nVehicleNameParaglider)
        return;

    *reinterpret_cast<float*>(regs.ebx + nVehicleUnderWaterMaxDepth) = fParagliderUnderWaterMaxDepth;
}

class Glider
{
public:
    Glider()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Gunfire knocking the glider out of the sky: the mass, set after the factory's second allocation.
            if (auto* p = dunia_find("53 56 57 6A 00 68 B0 02 00 00 E8 ? ? ? ? 83 C4 08 85 C0 74 0B 8B C8 E8 ? ? ? ? 8B F8 EB 02 33 FF 6A 00 6A 60", 0x30))
                ParagliderMassHook = safetyhook::create_mid(p, SetParagliderMass);

            // Bouncing on water, first half: OnDiscarded, on the FLD of the discarded mass.
            if (auto* p = dunia_find("56 8B F1 83 7E 64 00 74 29 8B 46 60 8B 08 8B 11 8B 42 08 FF D0 83 F8 07 75 18 8B 4E 60 8B 09 85 C9 74 0F D9 86 20 02 00 00 51 D9 1C 24", 0x23))
                ParagliderDiscardHook = safetyhook::create_mid(p, SetDiscardedMass);

            // Bouncing on water, second half: IsSubmerged, on the seven byte COMISS, which then reads
            // the rewritten depth.
            if (auto* p = dunia_find("0F 57 C0 83 EC 0C 53 8B D9 0F 2F 83 14 01 00 00 73 63 8B 43 08 56 8B 70 08 83 46 08 01 57 8B 7E 0C 8B CF", 9))
                VehicleSubmergedHook = safetyhook::create_mid(p, SetUnderWaterMaxDepth);
        };
    }
} Glider;
