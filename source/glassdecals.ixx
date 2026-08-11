/*
  "Windshield bullet holes are now permanent", in code. Credit to Boggalog.

  The data version is a one-attribute edit to databases/generic/decal.xml, which has to ride in
  patch.dat. The entry is Base.Glass, the bullet impact decal every windshield hit spawns:
  fLifeTime 20, fFadeInDuration 0, fFadeOutDuration 3.

  SDecalDescription is a 0x154 byte object (factory FUN_105271D0, constructor FUN_105D56C0, schema
  FUN_105D4CD0) with crc_hidName at +0x0C and fLifeTime at +0x68. fLifeTime goes through the generic
  float property descriptor vtable 0x10E98014, whose serialiser renderconfig.ixx already hooks, so
  the property system is not the way in.

  The choke point is SDecalDescription::OnSerializationEvent (FUN_105D45E0), the post-load callback
  the schema registers under "SerializationEvent". After it returns the value lives in two places
  and both matter. CDecalManager::SpawnDecal reads desc+0x68 to stamp each decal's expiry (now +
  fLifeTime + fFadeOutDuration, at +0x0C of the 0x40 byte record) and CDecalManager::Update destroys
  the decal against that, while OnSerializationEvent has already copied desc+0x68 to material+0x90
  (then +0x94 fadeIn, +0x98 1/fadeIn, +0x9C 1/fadeOut) where it drives the shader fade. Patching one
  leaves an invisible hole holding a pool slot. Writing desc+0x68 on entry fixes both with one
  store.

  The value has to stay finite. The pool is capped (defaultrenderconfig.xml MaxDecalCount, stock
  200) and CDecalManager::EvictOldest scans for the smallest expiry, seeding its running minimum
  from FLT_MAX at 0x10E125CC and its victim pointer from the end of the record array. The compare is
  strictly-less-than, so with every live record at or past FLT_MAX, or NaN, the victim stays at the
  end iterator and the function reads and then writes through it. A million seconds keeps a float
  spacing of a sixteenth of a second, so glass decals still order correctly among themselves.

  Permanent here means never times out. Eviction is by soonest expiry, so glass is now the last
  thing evicted and a full 200 slot pool still reclaims it. MaxDecalCount is the separate lever.
*/

module;

#include <common.hxx>

export module glassdecals;

import common;
import dunia;

// SDecalDescription.
static constexpr uintptr_t nDecalNameHash = 0x0C;
static constexpr uintptr_t nDecalLifeTime = 0x68;

// crc_hidName of the Base.Glass entry, written in decal.xml as decimal 3942833213.
static constexpr uint32_t nBaseGlass = 0xEB02DC3D;

// Finite on purpose, see EvictOldest above. Eleven and a half days of continuous game time.
static constexpr float fPermanentLifeTime = 1.0e6f;

// __fastcall with no stack arguments; the only incoming register use is MOV EDI, ECX at +0x0C.
static SafetyHookInline DecalPostLoadHook{};

static void __fastcall DecalPostLoad(uint8_t* pDescription, void* pEdx)
{
    // Before the original, so the value reaches both the expiry stamp and the shader constant.
    if (pDescription != nullptr && *reinterpret_cast<uint32_t*>(pDescription + nDecalNameHash) == nBaseGlass)
        *reinterpret_cast<float*>(pDescription + nDecalLifeTime) = fPermanentLifeTime;

    DecalPostLoadHook.fastcall(pDescription, pEdx);
}

class GlassDecals
{
public:
    GlassDecals()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // SDecalDescription::OnSerializationEvent. Entry through the first texture slot fetch,
            // stopping before the first call. No absolute addresses in that span.
            auto pattern = dunia_pattern("55 8B EC 83 E4 F0 83 EC 24 53 56 57 8B F9 83 7F 24 00 75 26 8D 44 24 20 8D 8F D0 00 00 00");
            if (pattern.empty())
                return;

            // The decal database is read once during startup, so no ini watch and no toggle.
            DecalPostLoadHook = safetyhook::create_inline(pattern.get_first(), DecalPostLoad);
        };
    }
} GlassDecals;
