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
// The four schemas are registered by FUN_103F56E0 (geometry), FUN_103F7240 (terrain),
// FUN_103F4600 (shadow) and FUN_103F7520 (ambient), each after the shared base FUN_103F42E0,
// which is why every derived field sits at 0x20 or above. Bools are stored as four byte ints and
// go through the integer serialiser, so every field below is either a float or an int32.
//
// Ultra High is the only geometry, shadow and terrain preset touched, and High the only ambient
// one, which is where the settings say they apply.
//
// The step 3 geometry and shadow numbers are Boggalog's, from Far Cry 2 Patched, where they ship
// as data edits to DefaultRenderConfig.xml inside patch.dat.
//
// One of Boggalog's edits is deliberately not reproduced. His <Geometry> ultrahigh block sets
// TerrainLodScale="0.1", but CRenderGeometryConfig registers no TerrainLodScale descriptor: the
// only TerrainLodScale in Dunia is CRenderTerrainConfig+0x2C, and the string at 0x10E5078C is
// referenced from FUN_103F7240 alone. The attribute is inert where he wrote it. The real one is
// driven by BeyondUltraTerrain below.

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
static constexpr uint32_t nGeometryRealTreeCapsMaxDistance = 0x48;
static constexpr uint32_t nGeometryRealTreesLodScale = 0x4C;
static constexpr uint32_t nGeometryClustersLodScale = 0x50;
static constexpr uint32_t nGeometryLodScale = 0x54;
static constexpr uint32_t nGeometryKillLodScale = 0x58;
static constexpr uint32_t nGeometryMaxDecalCount = 0x64;
static constexpr uint32_t nGeometryMaxDecalCountPerType = 0x68;
static constexpr uint32_t nGeometryRealTreeLeafMinSize = 0x74;
static constexpr uint32_t nGeometryRealTreeHLeafMinSize = 0x78;
static constexpr uint32_t nGeometryRealTreeNodeMinSize = 0x7C;
static constexpr uint32_t nGeometryRealTreeMinSizeShadowScale = 0x80;
static constexpr uint32_t nGeometryClusterObjectMinSize = 0x84;
static constexpr uint32_t nGeometryClusterObjectMinSizeShadowScale = 0x88;
static constexpr uint32_t nGeometrySceneObjectMinSize = 0x8C;
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
// MinZoomFactor. The schema serialises KillLodScale first of the two, so it is already in place
// when the pair is tested.
static constexpr float fStockKillLodScale = 1.0f;

// Stock shadow map resolution of the ultrahigh block. Also the default ShadowResolution, so the
// setting is a no-op until it is moved.
static constexpr int32_t nStockShadowMapSize = 2048;

// BeyondUltraGeometry. Four steps over the Ultra High geometry block, indexed by the setting:
//
//     0  the values the game ships
//     1  twice the vanilla draw distance
//     2  four times the vanilla draw distance
//     3  the most the block will take, which is Boggalog's set
//
// Two conventions run in opposite directions. LOD scales run backwards: the lower the value the
// further the detail survives, and 0 is the maximum, so doubling a distance halves the scale. The
// MinSize thresholds are the size below which an object is culled outright and halve the same way,
// bottoming out at 0. Only RealTreeCapsMaxDistance and the two decal counts are plain quantities
// that double.
//
// Two columns are pinned rather than scaled.
//
// KillLodScale holds at 0.7 from step 1 on. It is the distance at which an object stops being
// drawn at all, and below 0.7 scenery pops in on map 2, which is where Boggalog stops as well.
// Halving it would put step 1 past his maximum, so the column is clamped rather than left to
// overshoot.
//
// LodScale holds at its stock 1.0 for every step. Lowering it is what makes road surfaces break
// up, and no terrain setting compensates for it, so the column stays where the game put it.
//
// RealTreeNodeMinSize starts at 0.002, small enough that halving it changes nothing visible, so it
// goes straight to 0 at step 1.
struct GeometryStep
{
    float fRealTreeCapsMaxDistance;
    float fRealTreesLodScale;
    float fClustersLodScale;
    float fLodScale;
    float fKillLodScale;
    float fRealTreeLeafMinSize;
    float fRealTreeHLeafMinSize;
    float fRealTreeNodeMinSize;
    float fClusterObjectMinSize;
    float fSceneObjectMinSize;
    int32_t nMaxDecalCount;
    int32_t nMaxDecalCountPerType;
};

static constexpr GeometryStep GeometrySteps[] =
{
    //  CapsDist RealTrees Clusters   Lod    Kill    Leaf     HLeaf     Node   Cluster   Scene   Decals PerType
    {    100.0f,   1.0f,    0.8f,    1.0f,   1.0f,  0.02f,   0.015f,   0.002f,  0.02f,   0.01f,    200,     50 }, // 0 stock
    {    200.0f,   0.5f,    0.4f,    1.0f,   0.7f,  0.01f,   0.0075f,  0.0f,    0.01f,   0.005f,   400,    100 }, // 1 twice
    {    400.0f,   0.25f,   0.2f,    1.0f,   0.7f,  0.005f,  0.00375f, 0.0f,    0.005f,  0.0025f,  800,    200 }, // 2 four times
    {   1000.0f,   0.1f,    0.1f,    1.0f,   0.7f,  0.0f,    0.0f,     0.0f,    0.0f,    0.0f,    2000,   1000 }, // 3 maximum
};

static constexpr int32_t nMaxGeometryStep = static_cast<int32_t>(sizeof(GeometrySteps) / sizeof(GeometrySteps[0])) - 1;

// BeyondUltraShadows. Boggalog's Ultra High shadow set, less the two map sizes, which
// ShadowResolution owns. SunShadowFadeRange and SunShadowRange1 already carry his values in the
// stock ultrahigh block, so only three fields move.
static constexpr float fBeyondUltraSunShadowRange0 = 6.0f;   // stock 4
static constexpr float fBeyondUltraSunShadowRange2 = 135.0f; // stock 140, which flickers
static constexpr float fBeyondUltraLeavesShadowRatio = 1.0f; // stock 0.5

// Static ambient shadow distance, the Ambient High half of the same claim.
static constexpr float fBeyondUltraMaxHemiMapDistance = 512.0f; // stock 160

// BeyondUltraTerrain. The same four steps over the Ultra High terrain block. TerrainLodScale runs
// backwards like the geometry scales and bottoms out at 0; the two detail distances are plain
// quantities that double. Step 3 goes well past Boggalog, who only moves
// TerrainDetailBlendViewDistance from 64 to 128.
struct TerrainStep
{
    float fLodScale;
    float fDetailViewDistance;
    float fDetailBlendViewDistance;
};

static constexpr TerrainStep TerrainSteps[] =
{
    //  LodScale  ViewDist  BlendDist
    {     1.0f,     512.0f,     64.0f }, // 0 stock
    {     0.5f,    1024.0f,    128.0f }, // 1 twice
    {     0.25f,   2048.0f,    256.0f }, // 2 four times
    {     0.0f,    4096.0f,   4096.0f }, // 3 maximum
};

static constexpr int32_t nMaxTerrainStep = static_cast<int32_t>(sizeof(TerrainSteps) / sizeof(TerrainSteps[0])) - 1;

// Small object and vegetation shadows, credited to miru. Unconditional: it is a fix rather than a
// setting, and Boggalog leaves the ultrahigh block's 1.25s alone.
static constexpr float fFixedMinSizeShadowScale = 1.0f; // stock 1.25, bar RealTree which is 1.0

static int32_t nGeometryStep = 0;
static int32_t nTerrainStep = 0;
static bool bBeyondUltraShadows = false;
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

// ShadowResolution carries no range. The only thing guarded against is a value the device cannot
// use at all, which would otherwise leave the game with no shadow map rather than a large one.
static bool HasShadowResolution()
{
    return nShadowResolution > 0;
}

static bool WantsShadow()
{
    return bBeyondUltraShadows || (HasShadowResolution() && nShadowResolution != nStockShadowMapSize);
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
    return nOffset == nGeometryRealTreeCapsMaxDistance
        || nOffset == nGeometryRealTreesLodScale
        || nOffset == nGeometryClustersLodScale
        || nOffset == nGeometryLodScale
        || nOffset == nGeometryKillLodScale
        || nOffset == nGeometryMaxDecalCount
        || nOffset == nGeometryMaxDecalCountPerType
        || nOffset == nGeometryRealTreeLeafMinSize
        || nOffset == nGeometryRealTreeHLeafMinSize
        || nOffset == nGeometryRealTreeNodeMinSize
        || nOffset == nGeometryRealTreeMinSizeShadowScale
        || nOffset == nGeometryClusterObjectMinSize
        || nOffset == nGeometryClusterObjectMinSizeShadowScale
        || nOffset == nGeometrySceneObjectMinSize
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
    if (bBeyondUltraShadows)
    {
        SetFloat(pObject, nShadowSunShadowRange0, fBeyondUltraSunShadowRange0);
        SetFloat(pObject, nShadowSunShadowRange2, fBeyondUltraSunShadowRange2);
        SetFloat(pObject, nShadowLeavesShadowRatio, fBeyondUltraLeavesShadowRatio);
    }

    if (HasShadowResolution())
    {
        SetInt(pObject, nShadowShadowMapSize, nShadowResolution);
        SetInt(pObject, nShadowCascadedShadowMapSize, nShadowResolution);
    }
}

static void ApplyGeometry(uint8_t* pObject)
{
    const auto& step = GeometrySteps[nGeometryStep];

    SetFloat(pObject, nGeometryRealTreeCapsMaxDistance, step.fRealTreeCapsMaxDistance);
    SetFloat(pObject, nGeometryRealTreesLodScale, step.fRealTreesLodScale);
    SetFloat(pObject, nGeometryClustersLodScale, step.fClustersLodScale);
    SetFloat(pObject, nGeometryLodScale, step.fLodScale);
    SetFloat(pObject, nGeometryKillLodScale, step.fKillLodScale);
    SetFloat(pObject, nGeometryRealTreeLeafMinSize, step.fRealTreeLeafMinSize);
    SetFloat(pObject, nGeometryRealTreeHLeafMinSize, step.fRealTreeHLeafMinSize);
    SetFloat(pObject, nGeometryRealTreeNodeMinSize, step.fRealTreeNodeMinSize);
    SetFloat(pObject, nGeometryClusterObjectMinSize, step.fClusterObjectMinSize);
    SetFloat(pObject, nGeometrySceneObjectMinSize, step.fSceneObjectMinSize);
    SetInt(pObject, nGeometryMaxDecalCount, step.nMaxDecalCount);
    SetInt(pObject, nGeometryMaxDecalCountPerType, step.nMaxDecalCountPerType);

    SetFloat(pObject, nGeometryRealTreeMinSizeShadowScale, fFixedMinSizeShadowScale);
    SetFloat(pObject, nGeometryClusterObjectMinSizeShadowScale, fFixedMinSizeShadowScale);
    SetFloat(pObject, nGeometrySceneObjectMinSizeShadowScale, fFixedMinSizeShadowScale);
}

static void ApplyTerrain(uint8_t* pObject)
{
    const auto& step = TerrainSteps[nTerrainStep];

    SetFloat(pObject, nTerrainLodScale, step.fLodScale);
    SetFloat(pObject, nTerrainDetailViewDistance, step.fDetailViewDistance);
    SetFloat(pObject, nTerrainDetailBlendViewDistance, step.fDetailBlendViewDistance);
}

static void ApplyAmbient(uint8_t* pObject)
{
    SetFloat(pObject, nAmbientMaxHemiMapDistance, fBeyondUltraMaxHemiMapDistance);
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
        if (nTerrainStep > 0 && name == "TerrainAffectedByMuzzleFlash" && *(int32_t*)(pObject + nOffset) == nUltraAffectedByMuzzleFlash)
        {
            pUltraTerrain = pObject;
            ApplyTerrain(pObject);
            return;
        }
        break;

    case nAmbientSectorCountX:
        if (bBeyondUltraShadows && name == "SectorCountX" && *(int32_t*)(pObject + nOffset) == nHighSectorCountX)
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
            // Already bounded when the ini is read; bounded again so each table lookup is safe on
            // its own terms.
            const auto nGeometry = JackalFixSettings.GetInt(PREF_BEYONDULTRAGEOMETRY);
            nGeometryStep = nGeometry < 0 ? 0 : (nGeometry > nMaxGeometryStep ? nMaxGeometryStep : nGeometry);

            const auto nTerrain = JackalFixSettings.GetInt(PREF_BEYONDULTRATERRAIN);
            nTerrainStep = nTerrain < 0 ? 0 : (nTerrain > nMaxTerrainStep ? nMaxTerrainStep : nTerrain);

            bBeyondUltraShadows = JackalFixSettings.GetInt(PREF_BEYONDULTRASHADOWS) != 0;
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
