/*
  "Windshield bullet holes are now permanent", in code. Credit to Boggalog.

  The data version is a one-attribute edit to databases/generic/decal.xml, which has to ride in
  patch.dat. The entry is the glass bullet impact, the decal every windshield hit spawns:

    <Generic Name="Base.Glass" hidName="Base.Glass" crc_hidName="3942833213"
             texDiffuse="graphics\GFX\_Decals\GFXD_bullimp_glass.XBT" ...
             fLifeTime="20" fFadeInDuration="0" fFadeOutDuration="3" ... />

  Twenty seconds, then a three second fade, and the windscreen you just shot out is clean again.

  SDecalDescription is a 0x154 byte object (factory FUN_105271D0, constructor FUN_105D56C0, schema
  FUN_105D4CD0) with crc_hidName at +0x0C and fLifeTime at +0x68. fLifeTime goes through the generic
  float property descriptor vtable 0x10E98014, whose serialiser renderconfig.ixx already hooks, so
  the property system is not the way in.

  It does not need to be. The value has exactly one choke point between the XML and everything that
  reads it: SDecalDescription::OnSerializationEvent (FUN_105D45E0), the post-load callback the
  schema registers under "SerializationEvent" and the only thing that ever calls it. It resolves the
  three texture paths, creates the paired CSceneDecalMaterial, and bakes the authored values into
  that material's shader constants - including the timing quartet:

    FLD   dword ptr [EDI+0x68]        ; desc->fLifeTime
    FSTP  dword ptr [ESI+0x90]        ; material->m_fLifeTime
    ...                               ; +0x94 fadeIn, +0x98 1/fadeIn, +0x9C 1/fadeOut

  After it returns the number lives in two places, and both matter. CDecalManager::SpawnDecal reads
  desc+0x68 to stamp each decal's expiry (now + fLifeTime + fFadeOutDuration, at +0x0C of the 0x40
  byte record), and CDecalManager::Update compares that against the clock and destroys the decal.
  Meanwhile material+0x90 drives the shader's fade. Rewriting the descriptor anywhere later - or
  intercepting the expiry comparison - fixes one and not the other, and a decal that never expires
  but has already faded to nothing is worse than no fix: an invisible hole holding a pool slot.

  Writing desc+0x68 on entry, before either consumer has seen it, fixes both with one store, once,
  at load, and costs nothing per frame.

  On the value. The obvious choice is infinity, and it would crash the game. The pool is capped
  (defaultrenderconfig.xml MaxDecalCount, stock 200) and CDecalManager::EvictOldest picks a victim
  by scanning for the smallest expiry, seeding its running minimum from FLT_MAX at 0x10E125CC and
  its victim pointer from the end of the record array:

    MOVSS  XMM1, [0x10E125CC]     ; FLT_MAX
    MOV    ESI, EAX               ; victim = end
    MOVSS  XMM0, [ECX+0x0C]       ; record->expireTime
    COMISS XMM1, XMM0
    JBE    keep                   ; strictly-less-than, so FLT_MAX never wins
    MOV    ESI, ECX

  If every live record were at or past FLT_MAX, or NaN, nothing would ever beat the seed, ESI would
  stay at the end iterator, and the function would read and then write through it. So the lifetime
  has to stay finite and comfortably below FLT_MAX. A million seconds is eleven and a half days of
  continuous play, and its float spacing is a sixteenth of a second, so glass decals still order
  correctly among themselves and the recycler keeps behaving like a proper least-recently-used
  queue when the pool does fill.

  Which is the honest caveat: permanent means "never times out", not "never recycled". Because
  eviction is by soonest expiry, glass is now the last thing evicted, so once enough windscreens
  have been shot the 200 slot pool will start reclaiming glass decals as new ones arrive. Raising
  MaxDecalCount is the separate lever for that.
*/

module;

#include <common.hxx>

export module glassdecals;

import common;
import dunia;

// SDecalDescription.
static constexpr uintptr_t nDecalNameHash = 0x0C;
static constexpr uintptr_t nDecalLifeTime = 0x68;

// crc_hidName of the Base.Glass entry, straight out of decal.xml where it is written in decimal as
// 3942833213. This is the bullet impact decal for glass, which is what a windscreen is made of.
static constexpr uint32_t nBaseGlass = 0xEB02DC3D;

// Finite on purpose - see the note above about EvictOldest and FLT_MAX. Eleven and a half days of
// continuous game time, which no session reaches.
static constexpr float fPermanentLifeTime = 1.0e6f;

// __fastcall with no stack arguments: the function's only use of an incoming register is
// MOV EDI, ECX at +0x0C.
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

            // The decal database is read once during startup, so nothing here is registered on the
            // ini watch - and this is a bug fix rather than a preference, so there is nothing to
            // toggle either.
            DecalPostLoadHook = safetyhook::create_inline(pattern.get_first(), DecalPostLoad);
        };
    }
} GlassDecals;
