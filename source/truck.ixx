/*
  "Bug fix - Silent big truck engine" from An Almost Complete Guide to Far Cry 2 Modding, in code.

  The guide's version is a data edit to entitylibrarypatchoverride.fcb, needing a repacked
  patch.dat: the Land.BigTruck* engine sounds point at ids that produce almost nothing, and it
  repoints the seven in SoundRemaps below across five prototypes (Land.BigTruck,
  .A2LM09_NitrousTruck, .ScriptedBigTruck, .Tanker and Land.BigTruck_Tanker).

  They are indices 0, 1, 2, 4, 5, 6 and 10 of uint32_t m_SoundIds[21] at +0x2A0 of the vehicle
  engine and FX component (schema FUN_101AF0C0, constructor FUN_101ACFE0, vtable 0x10E266EC).
  FUN_100F7420 is their only deserialiser (descriptor vtables 0x10E1C290 and 0x10E26670), sscanfing
  the archive's literal "0x0045CD73" text into object + desc->offset + desc->index * 4 at entity
  library load. Different function and vtable from the float serialiser renderconfig.ixx hooks, so
  the two do not collide.

  Dunia.dll contains no prototype names and the class carries no RTTI, so the truck is identified by
  the id values themselves: each occurs exactly once per big truck prototype and nowhere else in the
  shipped vehicle library. The 0x2A0 field offset gate confines the remap to this class; the other
  three sound-name schemas use 0x268/0x2D8, 0x224 and 0x58-0x6C/0x2C.
*/

module;

#include <common.hxx>

export module truck;

import common;
import dunia;

// Sound-name property descriptor. Six dwords rather than the usual five: array properties put the
// entry index between the field offset and the metadata.
struct SoundProperty
{
    void* pVTable;              // 0x00  one per property type
    const char* pszName;        // 0x04
    uint32_t nNameHash;         // 0x08
    uint32_t nFieldOffset;      // 0x0C  base of the sound id array within the object
    uint32_t nElementIndex;     // 0x10  which entry of that array
    void* pMetadata;            // 0x14
};

static_assert(offsetof(SoundProperty, nFieldOffset) == 0x0C);
static_assert(offsetof(SoundProperty, nElementIndex) == 0x10);

// uint32_t m_SoundIds[21], per the constructor's fill loop.
static constexpr uint32_t nVehicleSoundIds = 0x2A0;
static constexpr uint32_t nVehicleSoundIdCount = 21;

struct SoundRemap
{
    uint32_t nSilent;
    uint32_t nAudible;
};

// One entry per distinct id: sndStopEngineIdleLoop and sndTurnOffEngine share both halves of their
// mapping, so the guide's seven changes are six pairs here.
static constexpr SoundRemap SoundRemaps[] =
{
    { 0x0045CD73, 0x004EE930 },  // index 0,  sndPlayEngineIdleLoop
    { 0x0045CD7A, 0x004EE933 },  // index 1,  sndStopEngineIdleLoop / index 5, sndTurnOffEngine
    { 0x0045CD74, 0x004EE931 },  // index 2,  sndEngineLoop
    { 0x0045CD79, 0x004EE932 },  // index 4,  sndEngineIgnition
    { 0x004B8893, 0x004EE940 },  // index 6,  sndFrameLoop
    { 0x0045CD71, 0x004EE92F },  // index 10, sndThrustPedal
};

// Runs after the loader has parsed the id out of the archive and written it into the array. An
// omitted property leaves the field at the constructor's 0xFFFFFFFF, which matches nothing here.
static void OnSoundIdRead(const SoundProperty* pProperty, uint8_t* pObject)
{
    if (pProperty == nullptr || pObject == nullptr)
        return;

    if (pProperty->nFieldOffset != nVehicleSoundIds || pProperty->nElementIndex >= nVehicleSoundIdCount)
        return;

    auto& nSoundId = *reinterpret_cast<uint32_t*>(pObject + nVehicleSoundIds + pProperty->nElementIndex * 4);

    for (const auto& remap : SoundRemaps)
    {
        if (nSoundId == remap.nSilent)
        {
            nSoundId = remap.nAudible;
            return;
        }
    }
}

// __thiscall with two stack arguments and a callee cleanup of eight bytes.
static SafetyHookInline SoundIdReadHook{};

static void __fastcall SoundIdRead(const SoundProperty* pProperty, void* pEdx, uint8_t* pObject, void* pArchive)
{
    SoundIdReadHook.fastcall(pProperty, pEdx, pObject, pArchive);
    OnSoundIdRead(pProperty, pObject);
}

class Truck
{
public:
    Truck()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // FUN_100F7420, entry to the parse call. No absolute addresses in that span (0xD8 is
            // the archive's ReadString vtable slot) and it stops before the call displacement.
            if (auto* p = dunia_find("56 8B C1 8B 48 10 8B 50 0C 57 8D 34 8A 8B 4C 24 10 8B 11 03 74 24 0C 8D 7C 24 0C 83 C0 04 57 50 8B 82 D8 00 00 00 FF D0 84 C0 74 0F 8B 4C 24 0C 51"))
            {
                // The entity library is read once during startup, so no ini watch and no toggle.
                SoundIdReadHook = safetyhook::create_inline(p, SoundIdRead);
            }
        };
    }
} Truck;
