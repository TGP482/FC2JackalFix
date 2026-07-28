/*
  "Bug fix - Silent big truck engine" from An Almost Complete Guide to Far Cry 2 Modding, in code.

  The guide's version is a data edit: every Land.BigTruck* prototype in
  entitylibrarypatchoverride.fcb points its engine sounds at a set of ids that produce almost
  nothing, and the fix repoints seven of them at a set that does:

    sndPlayEngineIdleLoop   0x0045CD73 -> 0x004EE930
    sndStopEngineIdleLoop   0x0045CD7A -> 0x004EE933
    sndEngineLoop           0x0045CD74 -> 0x004EE931
    sndEngineIgnition       0x0045CD79 -> 0x004EE932
    sndTurnOffEngine        0x0045CD7A -> 0x004EE933
    sndFrameLoop            0x004B8893 -> 0x004EE940
    sndThrustPedal          0x0045CD71 -> 0x004EE92F

  Five prototypes need it - Land.BigTruck, .A2LM09_NitrousTruck, .ScriptedBigTruck, .Tanker and
  Land.BigTruck_Tanker - and all of it needs a repacked patch.dat.

  These are properties of the vehicle engine and FX component, whose schema FUN_101AF0C0 builds
  into the root at 0x10FE7A64 (accessor 0x101B0CE0, class vtable 0x10E266EC, constructor
  FUN_101ACFE0). They are not seven separate fields: every sound property in that class is
  registered against field offset 0x2A0 with its own array index, and the constructor fills
  twenty-one dwords from +0x2A0 with 0xFFFFFFFF. So the class holds uint32_t m_SoundIds[21] at
  0x2A0, and the seven the guide edits are indices 0, 1, 2, 4, 5, 6 and 10.

  In the archive the values are the literal text "0x0045CD73". The property type that reads them
  (descriptor vtable 0x10E1C290, plus 0x10E26670 for the _FrontLeft and wheel aliases, which write
  the same array) has one deserialiser, FUN_100F7420, and nothing else in the image reaches it:

    MOV  ECX, [EAX+0x10]           ; desc->index
    MOV  EDX, [EAX+0x0C]           ; desc->offset, 0x2A0 here
    LEA  ESI, [EDX+ECX*4]          ; four bytes per entry
    ADD  ESI, [ESP+0x0C]           ; + object
    ...  archive->ReadString(&desc->name, &text)
    TEST AL, AL
    JZ   absent                    ; property not in this prototype, field left alone
    CALL FUN_10621520              ; sscanf(text, "0x%08X")
    MOV  [ESI], EAX

  Which makes it the natural place to intervene: it is the same code path the archive edit flows
  through, it runs once per property at entity library load rather than per frame, and it is
  narrow - two vtables reach it, both of them sound-name property types. Note it is not the
  generic float serialiser renderconfig.ixx hooks; that is a different function on a different
  vtable, so the two do not collide.

  Identifying the truck is the interesting part. Dunia.dll contains no prototype names at all -
  they live in the archive - and the class carries no RTTI, so there is nothing to match a name
  against. The values themselves are the discriminator instead, and they are a good one:

    - The seven stock ids only ever appear on big truck engine fields. Checked against the shipped
      vehicle library: each of the six distinct ids occurs exactly once per big truck prototype
      and on no other entity and no other field.
    - Matching on value covers all five prototypes without enumerating them, whatever they inherit
      from whatever parent.
    - A collision would be harmless in the direction that matters. These ids are the reason the
      truck is silent, so anything else pointing at them is silent too, and would be repointed at
      a working engine loop rather than broken.

  It is still gated on the descriptor's field offset being 0x2A0, which is what confines the remap
  to this class: of the four schemas that register sound-name properties, the other three use
  0x268/0x2D8, 0x224 and 0x58-0x6C/0x2C. So a stray id elsewhere in the engine cannot be caught by
  the value match alone.
*/

module;

#include <common.hxx>

export module truck;

import common;
import dunia;

// Sound-name property descriptor. Six dwords rather than the usual five: these are array
// properties, so the entry index sits between the field offset and the metadata.
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

// One entry per distinct id rather than per property: sndStopEngineIdleLoop and sndTurnOffEngine
// share both halves of their mapping, which is why the guide lists seven changes but only six
// pairs. Keyed by value, so the index each one lands on does not need naming.
static constexpr SoundRemap SoundRemaps[] =
{
    { 0x0045CD73, 0x004EE930 },  // index 0,  sndPlayEngineIdleLoop
    { 0x0045CD7A, 0x004EE933 },  // index 1,  sndStopEngineIdleLoop / index 5, sndTurnOffEngine
    { 0x0045CD74, 0x004EE931 },  // index 2,  sndEngineLoop
    { 0x0045CD79, 0x004EE932 },  // index 4,  sndEngineIgnition
    { 0x004B8893, 0x004EE940 },  // index 6,  sndFrameLoop
    { 0x0045CD71, 0x004EE92F },  // index 10, sndThrustPedal
};

// Called once per sound property of every prototype that has one, after the loader has parsed the
// id out of the archive and written it into the array. A property the prototype omits leaves the
// field at the constructor's 0xFFFFFFFF, which matches nothing here.
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

// __thiscall with two stack arguments and a callee cleanup of eight bytes, which is what
// __fastcall produces here once the unused edx slot is declared.
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
            // The sound-name deserialiser, from its entry to the parse call. No absolute
            // addresses in that span - the 0xD8 is the archive's ReadString vtable slot - and the
            // pattern stops before the call displacement.
            auto pattern = dunia_pattern("56 8B C1 8B 48 10 8B 50 0C 57 8D 34 8A 8B 4C 24 10 8B 11 03 74 24 0C 8D 7C 24 0C 83 C0 04 57 50 8B 82 D8 00 00 00 FF D0 84 C0 74 0F 8B 4C 24 0C 51");
            if (pattern.empty())
                return;

            // The entity library is read once during startup, so nothing here is registered on the
            // ini watch - and this is a bug fix rather than a preference, so there is nothing to
            // toggle either.
            SoundIdReadHook = safetyhook::create_inline(pattern.get_first(), SoundIdRead);
        };
    }
} Truck;
