module;

#include <common.hxx>

export module renderconfig;

import common;
import dunia;
import settings;

// engine\settings\DefaultRenderConfig.xml is not parsed attribute by attribute. Every render
// config class (CRenderShadowConfig, CRenderGeometryConfig, CRenderTerrainConfig,
// CRenderAmbientConfig) publishes a schema of property descriptors through its vtable: name, name
// hash, byte offset of the field. The loader calls each descriptor's serialise method, which
// resolves object + descriptor->offset and hands the address to the visitor, the XML reader.
//
// Only two numeric serialisers exist, float and int, shared by every config class. Hooking both
// after they return covers every numeric attribute of every <quality> block with the descriptor
// still in hand, so nothing here depends on patch.dat.
//
// Two of the values written here are fixes ported from Scrubah's Patch, which ships them as data
// edits to engine\settings\DefaultRenderConfig.xml inside patch.dat:
//
//     <Geometry> ultrahigh   ClusterObjectMinSizeShadowScale   1.25 -> 1.0
//                            SceneObjectMinSizeShadowScale     1.25 -> 1.0
//     <Terrain>  ultrahigh   TerrainDetailViewDistance          512 -> 4096
//                            TerrainDetailBlendViewDistance      64 -> 4096

// Property descriptor, as built by the schema registration functions.
struct RenderProperty
{
    void* pVTable;              // 0x00  one vtable per field type
    const char* pszName;        // 0x04  the XML attribute name
    uint32_t nNameHash;         // 0x08
    uint32_t nFieldOffset;      // 0x0C  byte offset of the field inside the config object
    void* pMetadata;            // 0x10  description, editor range, console visibility
};

static_assert(offsetof(RenderProperty, pszName) == 0x04);
static_assert(offsetof(RenderProperty, nFieldOffset) == 0x0C);

// CRenderShadowConfig - <Shadow><quality>
static constexpr uint32_t nShadowSunShadowFadeRange = 0x3C;
static constexpr uint32_t nShadowSunShadowRange0 = 0x40;
static constexpr uint32_t nShadowSunShadowRange1 = 0x44;
static constexpr uint32_t nShadowSunShadowRange2 = 0x48;
static constexpr uint32_t nShadowShadowMapSize = 0x4C;
static constexpr uint32_t nShadowCascadedShadowMapSize = 0x50;
static constexpr uint32_t nShadowLeavesShadowRatio = 0x60;

// CRenderGeometryConfig - <Geometry><quality>
static constexpr uint32_t nGeometryRealTreesLodScale = 0x4C;
static constexpr uint32_t nGeometryClustersLodScale = 0x50;
static constexpr uint32_t nGeometryLodScale = 0x54;
static constexpr uint32_t nGeometryKillLodScale = 0x58;
static constexpr uint32_t nGeometryRealTreeMinSizeShadowScale = 0x80;
static constexpr uint32_t nGeometryClusterObjectMinSizeShadowScale = 0x88;
static constexpr uint32_t nGeometrySceneObjectMinSizeShadowScale = 0x94;
static constexpr uint32_t nGeometryMinZoomFactor = 0x9C;

// CRenderTerrainConfig - <Terrain><quality>
static constexpr uint32_t nTerrainLodScale = 0x2C;
static constexpr uint32_t nTerrainDetailViewDistance = 0x30;
static constexpr uint32_t nTerrainDetailBlendViewDistance = 0x34;
static constexpr uint32_t nTerrainAffectedByMuzzleFlash = 0x38;

// CRenderAmbientConfig - <Ambient><quality>
static constexpr uint32_t nAmbientMaxHemiMapDistance = 0x30;
static constexpr uint32_t nAmbientSectorCountX = 0x3C;

// The quality id is the key of the map the parsed blocks are stored in, not a field on the block,
// so the serialiser cannot see it. Each section is recognised instead by one property whose stock
// value is unique to the preset the Ultra High profile selects: "ultrahigh" for ShadowQuality,
// GeometryQuality and TerrainQuality, "high" for AmbientQuality. Lower presets never carry these
// values and are left alone.
static constexpr int32_t nUltraShadowMapSize = 2048;      // 16, 341, 680, 1364, 1364, 2048, 720, 720
static constexpr float fUltraMinZoomFactor = 0.10f;       // 0.70, 0.60, 0.45, 0.35, 0.225, 0.10
static constexpr int32_t nUltraAffectedByMuzzleFlash = 1; // only the ultrahigh terrain block sets it
static constexpr int32_t nHighSectorCountX = 12;          // 4, 8, 8, 12, 8

// Stock KillLodScale of the ultrahigh geometry block, used to confirm the block alongside
// MinZoomFactor.
static constexpr float fStockKillLodScale = 1.0f;

// Stock shadow map resolution of the ultrahigh block. Also the default ShadowResolution, so the
// setting is a no-op until it is raised.
static constexpr int32_t nStockShadowMapSize = 2048;

// EnhancedLODs. LOD scales run backwards: the lower the value the further the detail survives,
// and 0 is the maximum.
static constexpr float fEnhancedKillLodScale = 0.7f;      // stock 1.0, below 0.7 pops on map 2
static constexpr float fEnhancedLodScale = 1.0f;          // stock 1.0
static constexpr float fEnhancedTerrainLodScale = 0.1f;   // stock 1.0, 
static constexpr float fEnhancedRealTreesLodScale = 0.0f; // stock 1.0
static constexpr float fEnhancedClustersLodScale = 0.0f;  // stock 0.8

// EnhancedShadowRange.
static constexpr float fEnhancedSunShadowFadeRange = 12.0f; // stock 10
static constexpr float fEnhancedSunShadowRange0 = 20.0f;    // stock 4
static constexpr float fEnhancedSunShadowRange1 = 40.0f;    // stock 20
static constexpr float fEnhancedSunShadowRange2 = 135.0f;   // stock 140, which flickers
static constexpr float fEnhancedLeavesShadowRatio = 1.0f;   // stock 0.5
static constexpr float fEnhancedMaxHemiMapDistance = 512.0f;// stock 160

// Small object and vegetation shadows, from Scrubah's Patch, credited there to miru.
static constexpr float fFixedMinSizeShadowScale = 1.0f; // stock 1.25

// Road detail texture distance, from Scrubah's Patch.
static constexpr float fFixedTerrainDetailViewDistance = 4096.0f;      // stock 512
static constexpr float fFixedTerrainDetailBlendViewDistance = 4096.0f; // stock 64

static bool bEnhancedLODs = false;
static bool bEnhancedShadowRange = false;
static int32_t nShadowResolution = nStockShadowMapSize;

static thread_local const void* pUltraShadow = nullptr;
static thread_local const void* pUltraGeometry = nullptr;
static thread_local const void* pUltraTerrain = nullptr;
static thread_local const void* pHighAmbient = nullptr;

static void SetFloat(uint8_t* pObject, uint32_t nOffset, float fValue)
{
    *(float*)(pObject + nOffset) = fValue;
}

static void SetInt(uint8_t* pObject, uint32_t nOffset, int32_t nValue)
{
    *(int32_t*)(pObject + nOffset) = nValue;
}

static bool WantsShadow()
{
    return bEnhancedShadowRange || nShadowResolution != nStockShadowMapSize;
}

static constexpr bool IsShadowField(uint32_t nOffset)
{
    return nOffset == nShadowSunShadowFadeRange
        || nOffset == nShadowSunShadowRange0
        || nOffset == nShadowSunShadowRange1
        || nOffset == nShadowSunShadowRange2
        || nOffset == nShadowShadowMapSize
        || nOffset == nShadowCascadedShadowMapSize
        || nOffset == nShadowLeavesShadowRatio;
}

static constexpr bool IsGeometryField(uint32_t nOffset)
{
    return nOffset == nGeometryKillLodScale
        || nOffset == nGeometryLodScale
        || nOffset == nGeometryClustersLodScale
        || nOffset == nGeometryRealTreesLodScale
        || nOffset == nGeometryRealTreeMinSizeShadowScale
        || nOffset == nGeometryClusterObjectMinSizeShadowScale
        || nOffset == nGeometrySceneObjectMinSizeShadowScale;
}

static constexpr bool IsTerrainField(uint32_t nOffset)
{
    return nOffset == nTerrainLodScale
        || nOffset == nTerrainDetailViewDistance
        || nOffset == nTerrainDetailBlendViewDistance;
}

static void ApplyShadow(uint8_t* pObject)
{
    if (bEnhancedShadowRange)
    {
        SetFloat(pObject, nShadowSunShadowFadeRange, fEnhancedSunShadowFadeRange);
        SetFloat(pObject, nShadowSunShadowRange0, fEnhancedSunShadowRange0);
        SetFloat(pObject, nShadowSunShadowRange1, fEnhancedSunShadowRange1);
        SetFloat(pObject, nShadowSunShadowRange2, fEnhancedSunShadowRange2);
        SetFloat(pObject, nShadowLeavesShadowRatio, fEnhancedLeavesShadowRatio);
    }

    SetInt(pObject, nShadowShadowMapSize, nShadowResolution);
    SetInt(pObject, nShadowCascadedShadowMapSize, nShadowResolution);
}

static void ApplyGeometry(uint8_t* pObject)
{
    if (bEnhancedLODs)
    {
        SetFloat(pObject, nGeometryKillLodScale, fEnhancedKillLodScale);
        SetFloat(pObject, nGeometryLodScale, fEnhancedLodScale);
        SetFloat(pObject, nGeometryClustersLodScale, fEnhancedClustersLodScale);
        SetFloat(pObject, nGeometryRealTreesLodScale, fEnhancedRealTreesLodScale);
    }

    SetFloat(pObject, nGeometryRealTreeMinSizeShadowScale, fFixedMinSizeShadowScale);
    SetFloat(pObject, nGeometryClusterObjectMinSizeShadowScale, fFixedMinSizeShadowScale);
    SetFloat(pObject, nGeometrySceneObjectMinSizeShadowScale, fFixedMinSizeShadowScale);
}

static void ApplyTerrain(uint8_t* pObject)
{
    if (!bEnhancedLODs)
        return;

    SetFloat(pObject, nTerrainLodScale, fEnhancedTerrainLodScale);
    SetFloat(pObject, nTerrainDetailViewDistance, fFixedTerrainDetailViewDistance);
    SetFloat(pObject, nTerrainDetailBlendViewDistance, fFixedTerrainDetailBlendViewDistance);
}

static void ApplyAmbient(uint8_t* pObject)
{
    SetFloat(pObject, nAmbientMaxHemiMapDistance, fEnhancedMaxHemiMapDistance);
}

// Called once per numeric attribute, after the loader has written the parsed value into the field.
// Name and offset are both checked: ShadowMapSize exists in the Shadow section and again in the
// Ambient section, where it sizes the sector ambient map.
static void OnPropertySerialised(const RenderProperty* pProperty, uint8_t* pObject)
{
    if (pProperty == nullptr || pObject == nullptr || pProperty->pszName == nullptr)
        return;

    const auto nOffset = pProperty->nFieldOffset;
    const std::string_view name(pProperty->pszName);

    switch (nOffset)
    {
    case nShadowShadowMapSize:
        if (WantsShadow() && name == "ShadowMapSize" && *(int32_t*)(pObject + nOffset) == nUltraShadowMapSize)
        {
            pUltraShadow = pObject;
            ApplyShadow(pObject);
            return;
        }
        break;

    case nGeometryMinZoomFactor:
        // MinZoomFactor is the only geometry attribute the xenon and ps3 blocks leave out, so it
        // is paired with KillLodScale, which the schema reads first.
        if (name == "MinZoomFactor"
            && *(float*)(pObject + nOffset) == fUltraMinZoomFactor
            && *(float*)(pObject + nGeometryKillLodScale) == fStockKillLodScale)
        {
            pUltraGeometry = pObject;
            ApplyGeometry(pObject);
            return;
        }
        break;

    case nTerrainAffectedByMuzzleFlash:
        if (bEnhancedLODs && name == "TerrainAffectedByMuzzleFlash" && *(int32_t*)(pObject + nOffset) == nUltraAffectedByMuzzleFlash)
        {
            pUltraTerrain = pObject;
            ApplyTerrain(pObject);
            return;
        }
        break;

    case nAmbientSectorCountX:
        if (bEnhancedShadowRange && name == "SectorCountX" && *(int32_t*)(pObject + nOffset) == nHighSectorCountX)
        {
            pHighAmbient = pObject;
            ApplyAmbient(pObject);
            return;
        }
        break;

    default:
        break;
    }

    // Fields the schema reads after the block was recognised, which the loader has just put back
    // to the value in the XML. Only offsets this module owns are acted on, so a stale block
    // pointer cannot write into an unrelated object.
    if (pObject == pUltraShadow && IsShadowField(nOffset))
        ApplyShadow(pObject);
    else if (pObject == pUltraGeometry && IsGeometryField(nOffset))
        ApplyGeometry(pObject);
    else if (pObject == pUltraTerrain && IsTerrainField(nOffset))
        ApplyTerrain(pObject);
    else if (pObject == pHighAmbient && nOffset == nAmbientMaxHemiMapDistance)
        ApplyAmbient(pObject);
}

// __thiscall with two stack arguments and a callee cleanup of eight bytes.
static SafetyHookInline SerialiseFloatHook{};
static SafetyHookInline SerialiseIntHook{};

static void __fastcall SerialiseFloat(const RenderProperty* pProperty, void* pEdx, uint8_t* pObject, void* pVisitor)
{
    SerialiseFloatHook.fastcall(pProperty, pEdx, pObject, pVisitor);
    OnPropertySerialised(pProperty, pObject);
}

static void __fastcall SerialiseInt(const RenderProperty* pProperty, void* pEdx, uint8_t* pObject, void* pVisitor)
{
    SerialiseIntHook.fastcall(pProperty, pEdx, pObject, pVisitor);
    OnPropertySerialised(pProperty, pObject);
}

class RenderConfig
{
public:
    RenderConfig()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            bEnhancedLODs = JackalFixSettings.GetInt(PREF_ENHANCEDLODS) != 0;
            bEnhancedShadowRange = JackalFixSettings.GetInt(PREF_ENHANCEDSHADOWRANGE) != 0;
            nShadowResolution = JackalFixSettings.GetInt(PREF_SHADOWRESOLUTION);

            // No early out: the shadow scale fix has no setting, so the hooks always go in.

            // CFloatProperty::Serialise - esi = object + descriptor->offset, then a call through
            // the visitor's float slot at +0x10C.
            auto floatPattern = dunia_pattern("56 8B C1 8B 70 0C 03 74 24 08 8B 4C 24 0C 8B 11 83 C0 04 56 50 8B 82 0C 01 00 00 FF D0 5E C2 08 00");

            // CIntProperty::Serialise - the same shape through the visitor's int slot at +0x114.
            auto intPattern = dunia_pattern("56 8B C1 8B 70 0C 03 74 24 08 8B 4C 24 0C 8B 11 83 C0 04 56 50 8B 82 14 01 00 00 FF D0 5E C2 08 00");

            if (floatPattern.empty() || intPattern.empty())
                return;

            SerialiseFloatHook = safetyhook::create_inline(floatPattern.get_first(), SerialiseFloat);
            SerialiseIntHook = safetyhook::create_inline(intPattern.get_first(), SerialiseInt);

            // The render config is read once during startup, so nothing is registered on the file
            // watch; a change needs a restart.
        };
    }
} RenderConfig;
