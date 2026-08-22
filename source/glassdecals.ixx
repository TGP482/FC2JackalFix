/*
  "Windshield bullet holes are now permanent", in code. Credit to Boggalog.
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
            if (auto* p = dunia_find("55 8B EC 83 E4 F0 83 EC 24 53 56 57 8B F9 83 7F 24 00 75 26 8D 44 24 20 8D 8F D0 00 00 00"))
            {
                // The decal database is read once during startup, so no ini watch and no toggle.
                DecalPostLoadHook = safetyhook::create_inline(p, DecalPostLoad);
            }
        };
    }
} GlassDecals;
