/*
  Credit to https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer
  Some fixes were implemented based on the Far Cry 2 Multi Fixer by FoxAhead.
*/

module;

#include <common.hxx>
#include <atomic>

export module fov;

import common;
import dunia;
import settings;

// The camera controller stores base FOV at +0x70. Replacing the setter value changes world FOV
// while leaving weapon aiming FOV untouched.
static constexpr float fPi = 3.14159265f;

static float fFieldOfView = 75.0f;
static float fViewmodelScale = 1.0f;
static float fIronsightFieldOfView = 0.0f;
static float fVehicleFieldOfView = 0.0f;

// Scale the tangent so viewmodel FOV stays proportional and matches the in-game's widescreen scaling.
static float ScaleFov(float fovRad, float scale)
{
    if (scale == 1.0f)
        return fovRad;

    return 2.0f * std::atan(std::tan(fovRad * 0.5f) * scale);
}


// The engine's own widescreen stretch factor, the constant at 0x10EAF2F8.
static constexpr float fWidescreenStretch = 0.75f;

// Tangent-space zoom where viewmodel widening has fully faded out.
// Set below 1.0 so the transition is gradual instead of snapping as soon as zoom begins.
static constexpr float fAimFadeZoom = 0.85f;

// The near pass is handed whatever FOV the camera currently has, and that already includes ironsight
// zoom. Widening it by a fixed ratio is only correct at the hip: while aiming it draws the weapon at
// a wider FOV than the world, shrinking it on screen until the edges of the model - which were never
// meant to be on screen - come into frame. So the widening is faded out by how far the camera has
// actually zoomed, which needs no aim state and covers scopes and binoculars for free.
//
// Zoom is measured in tangent space against the unzoomed FOV, which is the configured FieldOfView
// after the widescreen stretch this fix now always applies. Above 1 means wider than base - a
// vehicle camera - and keeps the widening intact.
static float ViewmodelScaleFor(float fovRad, float aspect)
{
    auto fBaseTan = std::tan(fFieldOfView * (fPi / 360.0f)) * fWidescreenStretch * aspect;
    if (fBaseTan <= 0.0f)
        return fViewmodelScale;

    auto fZoom = std::tan(fovRad * 0.5f) / fBaseTan;
    auto fFade = std::clamp((fZoom - fAimFadeZoom) / (1.0f - fAimFadeZoom), 0.0f, 1.0f);

    return 1.0f + (fViewmodelScale - 1.0f) * fFade;
}

// Dunia identifies properties by CRC-32 hash rather than name, allowing
// properties to be intercepted without knowing their owning class or offset.
static constexpr uint32_t PropertyHash(std::string_view name)
{
    uint32_t nCrc = 0xFFFFFFFF;
    for (auto c : name)
    {
        nCrc ^= static_cast<uint8_t>(c);
        for (auto nBit = 0; nBit < 8; ++nBit)
            nCrc = (nCrc >> 1) ^ (0xEDB88320 & (0u - (nCrc & 1u)));
    }
    return ~nCrc;
}

// Sanity check that we're using Dunia's CRC variant.
static_assert(PropertyHash("fFOVAngle") == 0x49745480);

// Vehicle camera FOV (degrees).
static constexpr uint32_t nVehicleFovProperty = PropertyHash("fFOVAngle");

// Weapon ironsight FOV (radians). Converted separately from the vehicle FOV, which is stored in degrees.
static constexpr uint32_t nIronsightFovProperty = PropertyHash("fIronsightFOV");

// Only rewrite unmagnified sights, magnified optics use the same property.
static constexpr float fMagnifiedOpticCutoff = 40.0f;

// The property descriptor is overwritten before the second hook runs, so cache
// the property hash here. Thread-local because property streams are parsed on
// multiple threads.
static thread_local uint32_t nPendingProperty = 0;

// Map markers (archPlayerMarker, archDiamondMarker, etc.) are 3D
// entities owned by CCompassObjectives. On foot they render with the map, but
// in a vehicle the map moves to the world pass while the markers remain in the
// near pass. Viewmodel scaling makes this mismatch visible.
static constexpr uintptr_t nCompassInVehicleOffset = 0x28;

static std::atomic<bool> bMarkersInVehicle = false;
static std::atomic<uint32_t> nMarkersStamp = 0;

// Marker placement stops when the map is closed, so treat the vehicle flag as
// valid only briefly after the last marker update.
static constexpr uint32_t nMarkerFreshnessMs = 250;

static bool MapIsInVehicle()
{
    if (!bMarkersInVehicle.load(std::memory_order_relaxed))
        return false;

    return GetTickCount() - nMarkersStamp.load(std::memory_order_relaxed) <= nMarkerFreshnessMs;
}

class FieldOfView
{
public:
    FieldOfView()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Joshua - Old
            // fFOV setter: MOVSS XMM0,[ESP+4] / MULSS deg to rad / MOVSS [ECX+0x70]
            /*
            auto pattern = dunia_pattern("F3 0F 10 44 24 04 F3 0F 59 05 ? ? ? ? F3 0F 11 41 70 C2 04 00");
            if (pattern.empty())
                return;

            static auto FieldOfViewHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
            {
                *(float*)(regs.esp + 4) = fFieldOfView;
            });

            // Near pass uses a separate projection for weapons and arms, allowing viewmodel FOV changes
            // without affecting the world.
            pattern = dunia_pattern("D9 86 28 02 00 00 D9 1C 24 E8 ? ? ? ? D9 45 14");
            if (pattern.empty())
                return;

            static auto ViewmodelFovHook = safetyhook::create_mid(pattern.get_first(9), [](SafetyHookContext& regs)
            {
                auto pFov = (float*)regs.esp;
                *pFov = ScaleFov(*pFov, fViewmodelScale);
            });*/

            static auto FieldOfViewCB = []()
            {
                fFieldOfView = JackalFixSettings.GetFloat(PREF_FIELDOFVIEW);
                fViewmodelScale = std::tan(JackalFixSettings.GetFloat(PREF_VIEWMODELFIELDOFVIEW) * (fPi / 360.0f))
                                / std::tan(fFieldOfView * (fPi / 360.0f));
                fIronsightFieldOfView = JackalFixSettings.GetFloat(PREF_IRONSIGHTFIELDOFVIEW);
                fVehicleFieldOfView = JackalFixSettings.GetFloat(PREF_VEHICLEFIELDOFVIEW);
            };

            FieldOfViewCB();

            JackalFix::onIniFileChange() += []()
            {
                FieldOfViewCB();
            };

            // fFOV setter: MOVSS XMM0,[ESP+4] / MULSS deg to rad / MOVSS [ECX+0x70]
            auto fieldOfViewPattern = dunia_pattern("F3 0F 10 44 24 04 F3 0F 59 05 ? ? ? ? F3 0F 11 41 70 C2 04 00");
            if (!fieldOfViewPattern.empty())
            {
                static auto FieldOfViewHook = safetyhook::create_mid(fieldOfViewPattern.get_first(), [](SafetyHookContext& regs)
                {
                    *(float*)(regs.esp + 4) = fFieldOfView;
                });
            }

            // Near pass for the weapon and arms. Hook here to adjust the viewmodel FOV
            // independently of the world. ESI points to the camera; aspect ratio is at
            // +0x18.
            auto viewmodelPattern = dunia_pattern("D9 86 28 02 00 00 D9 1C 24 E8 ? ? ? ? D9 45 14");
            if (!viewmodelPattern.empty())
            {
                static auto ViewmodelFovHook = safetyhook::create_mid(viewmodelPattern.get_first(9), [](SafetyHookContext& regs)
                {
                    // In vehicles, keep the near-pass FOV at the world FOV so map markers stay aligned.
                    if (MapIsInVehicle())
                        return;

                    auto pFov = (float*)regs.esp;
                    *pFov = ScaleFov(*pFov, ViewmodelScaleFor(*pFov, *(float*)(regs.esi + 0x18)));
                });
            }

            // Hook at marker placement while ECX is the owning CCompassObjectives.
            // Read bInVehicle (+0x28) here instead of caching the component pointer.
            auto compassMarkerPattern = dunia_pattern("55 8B EC 83 E4 F0 81 EC 84 00 00 00 53 56 57 8B 7D 10 8B 77 0C 8B D9");
            if (!compassMarkerPattern.empty())
            {
                static auto CompassMarkerHook = safetyhook::create_mid(compassMarkerPattern.get_first(), [](SafetyHookContext& regs)
                {
                    bMarkersInVehicle.store(*(bool*)(regs.ecx + nCompassInVehicleOffset), std::memory_order_relaxed);
                    nMarkersStamp.store(GetTickCount(), std::memory_order_relaxed);
                });
            }

            // The Widescreen option controls whether the engine applies its built-in Hor+
            // FOV adjustment, but its blend weight can become stuck at zero after toggling
            // the setting, leaving the FOV unwidened until the game is restarted.
            //
            // Force the branch to always apply the engine's own widescreen adjustment and
            // pin the blend weight to 1.0. This preserves the engine's FOV math for normal,
            // ironsight, and vehicle cameras while making FieldOfView behave consistently
            // regardless of the menu setting. The weight is recomputed every frame, so this
            // write does not accumulate.
            auto widescreenPattern = dunia_pattern("8B 0D ? ? ? ? 83 79 40 00 0F 84 C4 00 00 00 E8 ? ? ? ? F3 0F 10 40 10");
            if (!widescreenPattern.empty())
            {
                static auto WidescreenFovHook = safetyhook::create_mid(widescreenPattern.get_first(10), [](SafetyHookContext& regs)
                {
                    static constexpr uintptr_t nZeroFlag = 0x40;

                    regs.eflags &= ~nZeroFlag;
                    *(float*)(regs.edi + 0x58) = 1.0f;
                });
            }

            // Weapon and vehicle FOV are loaded from FCB archives rather than computed at
            // runtime, so intercept property deserialization to override them. Install the
            // hook only when either option is enabled, since this is a hot load path and
            // changes require a restart anyway.
            if (fIronsightFieldOfView > 0.0f || fVehicleFieldOfView > 0.0f)
            {
                auto dataPropertyPattern = dunia_pattern("8B 54 24 04 8D 44 24 04 50 83 C2 04 52 E8 ? ? ? ? 85 C0 74 0D 8B 00 8B 4C 24 08 89 01 B0 01 C2 08 00");
                if (!dataPropertyPattern.empty())
                {
                    // Entry: the property descriptor is still available at [ESP+4]. Cache its hash
                    // now, as it is overwritten before the second hook.
                    static auto DataPropertyNameHook = safetyhook::create_mid(dataPropertyPattern.get_first(), [](SafetyHookContext& regs)
                    {
                        nPendingProperty = *(uint32_t*)(*(uintptr_t*)(regs.esp + 4) + 4);
                    });

                    // Property found. Replace the parsed value in EAX before it is written to the
                    // output, leaving the parser's bookkeeping untouched.
                    static auto DataPropertyFovHook = safetyhook::create_mid(dataPropertyPattern.get_first(0x18), [](SafetyHookContext& regs)
                    {
                        auto pValue = (float*)&regs.eax;

                        if (nPendingProperty == nIronsightFovProperty)
                        {
                            if (fIronsightFieldOfView > 0.0f && *pValue * (180.0f / fPi) >= fMagnifiedOpticCutoff)
                                *pValue = fIronsightFieldOfView * (fPi / 180.0f);
                        }
                        else if (nPendingProperty == nVehicleFovProperty)
                        {
                            if (fVehicleFieldOfView > 0.0f)
                                *pValue = fVehicleFieldOfView;
                        }
                    });
                }
            }
        };
    }
} FieldOfView;
