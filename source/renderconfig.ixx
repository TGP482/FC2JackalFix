module;

#include <common.hxx>
#include <cstdio>
#include <cstring>

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

// CRenderShadowConfig, from <Shadow><quality>
static constexpr uint32_t nShadowSunShadowFadeRange = 0x3C;
static constexpr uint32_t nShadowSunShadowRange0 = 0x40;
static constexpr uint32_t nShadowSunShadowRange1 = 0x44;
static constexpr uint32_t nShadowSunShadowRange2 = 0x48;
static constexpr uint32_t nShadowShadowMapSize = 0x4C;
static constexpr uint32_t nShadowCascadedShadowMapSize = 0x50;
static constexpr uint32_t nShadowLeavesShadowRatio = 0x60;

// CRenderGeometryConfig, from <Geometry><quality>
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

// CRenderTerrainConfig, from <Terrain><quality>
static constexpr uint32_t nTerrainLodScale = 0x2C;
static constexpr uint32_t nTerrainDetailViewDistance = 0x30;
static constexpr uint32_t nTerrainDetailBlendViewDistance = 0x34;
static constexpr uint32_t nTerrainAffectedByMuzzleFlash = 0x38;

// CRenderAmbientConfig, from <Ambient><quality>
static constexpr uint32_t nAmbientMaxHemiMapDistance = 0x30;
static constexpr uint32_t nAmbientSectorCountX = 0x3C;

// The quality id is the key of the map the parsed blocks are stored in rather than a field on the
// block, so the serialiser cannot see it. Each section is recognised instead by one property whose
// stock value is unique to the preset the Ultra High profile selects: "ultrahigh" for
// ShadowQuality, GeometryQuality and TerrainQuality, "high" for AmbientQuality. Lower presets
// never carry these values and are left alone.
static constexpr int32_t nUltraShadowMapSize = 2048;      // 16, 341, 680, 1364, 1364, 2048, 720, 720
static constexpr float fUltraMinZoomFactor = 0.10f;       // 0.70, 0.60, 0.45, 0.35, 0.225, 0.10
static constexpr int32_t nUltraAffectedByMuzzleFlash = 1; // only the ultrahigh terrain block sets it
static constexpr int32_t nHighSectorCountX = 12;          // 4, 8, 8, 12, 8

// Stock KillLodScale of the ultrahigh geometry block, used to confirm the block alongside
// MinZoomFactor. The schema serialises KillLodScale first of the two, so it is already in place
// when the pair is tested.
static constexpr float fStockKillLodScale = 1.0f;

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
    {   1000.0f,   0.1f,    0.1f,    0.0f,   0.7f,  0.0f,    0.0f,     0.0f,    0.0f,    0.0f,    2000,   1000 }, // 3 maximum
};

static constexpr int32_t nMaxGeometryStep = static_cast<int32_t>(sizeof(GeometrySteps) / sizeof(GeometrySteps[0])) - 1;

// BeyondUltraShadows. Boggalog's Ultra High shadow set, less the two map sizes, which are not
// touched at all: a shadow map is sized when the device builds it, so a setting for it could only
// ever be read at startup, and the one that was here never took effect. SunShadowFadeRange and
// SunShadowRange1 already carry his values in the stock ultrahigh block, so only three fields move.
static constexpr float fBeyondUltraSunShadowRange0 = 6.0f;   // stock 4
static constexpr float fBeyondUltraSunShadowRange2 = 999.0f; // stock 140, which flickers
static constexpr float fBeyondUltraLeavesShadowRatio = 1.0f; // stock 0.5

// Static ambient shadow distance, the Ambient High half of the same claim.
static constexpr float fBeyondUltraMaxHemiMapDistance = 512.0f; // stock 160

// The same four as the blocks ship them, kept because a setting that can be turned off while the
// game is running has to have somewhere to go back to, and the XML is long gone by then.
static constexpr float fStockSunShadowRange0 = 4.0f;
static constexpr float fStockSunShadowRange2 = 140.0f;
static constexpr float fStockLeavesShadowRatio = 0.5f;
static constexpr float fStockMaxHemiMapDistance = 160.0f;


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

// The four blocks, kept for the rest of the run rather than for the parse alone. They were
// thread_local and only remembered when a setting asked for them, which is right for catching the
// fields the schema reads after a block is recognised (one call to the loader, one thread) and
// wrong for a setting that moves while the game is running.
static const void* pUltraShadow = nullptr;
static const void* pUltraGeometry = nullptr;
static const void* pUltraTerrain = nullptr;
static const void* pHighAmbient = nullptr;

static void SetFloat(uint8_t* pObject, uint32_t nOffset, float fValue)
{
    *(float*)(pObject + nOffset) = fValue;
}

static void SetInt(uint8_t* pObject, uint32_t nOffset, int32_t nValue)
{
    *(int32_t*)(pObject + nOffset) = nValue;
}

static constexpr bool IsShadowField(uint32_t nOffset)
{
    return nOffset == nShadowSunShadowFadeRange
        || nOffset == nShadowSunShadowRange0
        || nOffset == nShadowSunShadowRange1
        || nOffset == nShadowSunShadowRange2
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

// Both directions, always. Writing the stock numbers back is what lets the setting be turned off
// without a restart, and while it has never been on this writes what is already there.
static void ApplyShadow(uint8_t* pObject)
{
    SetFloat(pObject, nShadowSunShadowRange0,
        bBeyondUltraShadows ? fBeyondUltraSunShadowRange0 : fStockSunShadowRange0);
    SetFloat(pObject, nShadowSunShadowRange2,
        bBeyondUltraShadows ? fBeyondUltraSunShadowRange2 : fStockSunShadowRange2);
    SetFloat(pObject, nShadowLeavesShadowRatio,
        bBeyondUltraShadows ? fBeyondUltraLeavesShadowRatio : fStockLeavesShadowRatio);
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
    SetFloat(pObject, nAmbientMaxHemiMapDistance,
        bBeyondUltraShadows ? fBeyondUltraMaxHemiMapDistance : fStockMaxHemiMapDistance);
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
        if (name == "ShadowMapSize" && *(int32_t*)(pObject + nOffset) == nUltraShadowMapSize)
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
        if (name == "TerrainAffectedByMuzzleFlash" && *(int32_t*)(pObject + nOffset) == nUltraAffectedByMuzzleFlash)
        {
            pUltraTerrain = pObject;
            ApplyTerrain(pObject);
            return;
        }
        break;

    case nAmbientSectorCountX:
        if (name == "SectorCountX" && *(int32_t*)(pObject + nOffset) == nHighSectorCountX)
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

static void TakeSettings()
{
    // Already bounded when the ini is read; bounded again so each table lookup is safe on its own
    // terms.
    const auto nGeometry = JackalFixSettings.GetInt(PREF_BEYONDULTRAGEOMETRY);
    nGeometryStep = nGeometry < 0 ? 0 : (nGeometry > nMaxGeometryStep ? nMaxGeometryStep : nGeometry);

    const auto nTerrain = JackalFixSettings.GetInt(PREF_BEYONDULTRATERRAIN);
    nTerrainStep = nTerrain < 0 ? 0 : (nTerrain > nMaxTerrainStep ? nMaxTerrainStep : nTerrain);

    bBeyondUltraShadows = JackalFixSettings.GetInt(PREF_BEYONDULTRASHADOWS) != 0;
}

/*
  Which of the game's own quality levels are in force, and getting the engine to look again.

  Everything above only ever touches the ultrahigh geometry, shadow and terrain blocks and the high
  ambient one, so a Beyond Ultra setting does nothing unless the game is running the preset it
  edits.

  A quality on CRenderProfile is NOT an int. Reading one as an int is what made every row read as
  ultra: the schema at Dunia+3F7F00 registers TerrainQuality at 114h, GeometryQuality at 130h,
  AmbientQuality at 14Ch and ShadowQuality at 168h, twelve of them 1Ch apart, which is the size of a
  string rather than of an int, and the property's own Serialise, the first slot of the quality
  vtable
  at Dunia+F7460, hands object+offset to the visitor's 0DCh slot rather than the 114h int slot the
  plain ints use. The display page proves what is there: Dunia+7779C0 reads each one as a string,
  buffer at field+4 and capacity at field+18h, taken by pointer instead once that capacity reaches
  sixteen, and puts the text through Dunia+3F2D30 to get a list position. So the field holds
  "ultrahigh", "veryhigh", "high", "medium" or "low", and the answer is a string compare.

  Making a change take hold is a second byte on the render manager. Dunia+354B30 sets +31h when it
  copies the profile in, and Dunia+33E540, the ordinary per-frame path on the engine's own thread,
  tests that byte, clears it and runs the functor at +13Ch, which is what re-reads the profile and
  picks the blocks up again. That is precisely what lowering the quality and putting it back was
  doing by hand. Setting the byte asks for the same thing without a device rebuild: +30h is the mode
  change and is deliberately left alone here.
*/
static constexpr uint32_t nProfileTerrainQuality = 0x114;
static constexpr uint32_t nProfileGeometryQuality = 0x130;
static constexpr uint32_t nProfileShadowQuality = 0x168;

// The rest of the same schema, kept only so the log can say whether a profile is the live one.
// Three names cannot tell a stale object from a true one, since a profile really can have Shadow
// alone off ultra, but the whole set can, because the display page shows every one of these and
// they cannot all be wrong at once by accident.
static constexpr uint32_t nProfilePostFxQuality = 0x6C;
static constexpr uint32_t nProfileTextureQuality = 0x88;
static constexpr uint32_t nProfileTextureResolutionQuality = 0xA4;
static constexpr uint32_t nProfileWaterQuality = 0xC0;
static constexpr uint32_t nProfileDepthPassQuality = 0xDC;
static constexpr uint32_t nProfileVegetationQuality = 0xF8;
static constexpr uint32_t nProfileAmbientQuality = 0x14C;

// The engine's string: four bytes of header, then sixteen of buffer, the length, and the capacity
// that says whether the buffer is the text or a pointer to it.
static constexpr ptrdiff_t nStringBuffer = 0x04;
static constexpr ptrdiff_t nStringCapacity = 0x18;
static constexpr uint32_t nStringLocalCapacity = 16;

static const char* const szUltraHigh = "ultrahigh";

/*
  Which CRenderProfile is the one the game is actually running.

  There are a dozen of them: low, medium, high, veryhigh, ultrahigh, optimal, custom, the d3d10
  variants of several, and the console ones. Every attempt to identify the right one by
  remembering what some hook last handed back has picked the wrong one. The log finally said why:

    live [custom customd3d10 custom customd3d10]
    preset [optimal ultrahigh ultrahighd3d10 ... optimal low low low low medium high veryhigh]

  Four live lookups, all of them at startup, and never another one. Applying a level in the display
  options does not modify the profile in force. It makes a DIFFERENT profile the one in force, and
  that one comes out of the preset store. So the object being read was the boot-time "custom"
  profile for the whole session, and the only reason Shadows ever greyed correctly is that the
  launcher config happened to leave it on high from the start. Everything else stayed at whatever
  the auto-detect wrote and never moved again, which is exactly what was on screen.

  The engine keeps no secret about which one it is. Dunia+402150 is the setter, and it is four
  instructions long:

    MOV ESI,ECX
    LEA ECX,[ESI+04h]        ; the name of the profile in force
    CALL <string assign>
    ...
    CALL <the render settings broadcast>

  So the name lives at +04h on the global profile, as a plain std::string, and it is written every
  time the level changes. That is read directly, with no hook, no capture and nothing to go stale,
  and the
  profile it names is taken from a small table of everything the find has handed out, which is the
  one thing hooking that function is genuinely good for.
*/
static constexpr ptrdiff_t nProfileCurrentName = 0x04;

// 81 EC 6C 03 00 00 / PUSH EBP / MOV EBP,[ESP+374h] / PUSH ESI / PUSH EDI / PUSH EBP / MOV EDI,ECX.
static const char* const szProfileFindPattern =
    "81 EC 6C 03 00 00 55 8B AC 24 74 03 00 00 56 57 55 8B F9 E8";

static SafetyHookInline ProfileFindHook{};

// The slot Dunia keeps the global CRenderProfile in, which is both the manager the lookups are made
// against and the object the render settings broadcast is raised on. Resolved with the broadcast
// further down; declared here because the lookup hook is the first thing to want it.
static void** ppGlobalRenderProfile = nullptr;

// Declared here and defined with the rest of the guarded reads further down.
static bool IsReadable(const void* pAddress, size_t nLength);

// The engine's string, wherever one is: four bytes of header, then the buffer, and a capacity that
// says whether the buffer is the text or a pointer to it.
static const char* StringText(const void* pString)
{
    if (!IsReadable(pString, nStringCapacity + sizeof(uint32_t)))
        return nullptr;

    const auto* pBytes = static_cast<const uint8_t*>(pString);
    const auto nCapacity = *reinterpret_cast<const uint32_t*>(pBytes + nStringCapacity);

    const auto* pText = nCapacity < nStringLocalCapacity
        ? reinterpret_cast<const char*>(pBytes + nStringBuffer)
        : *reinterpret_cast<const char* const*>(pBytes + nStringBuffer);

    return IsReadable(pText, 1) ? pText : nullptr;
}

// Everything the find has ever handed out, by the name it was asked for. Small and fixed: there
// are a dozen profiles and the same handful of names is asked for over and over, so an entry is
// replaced rather than appended once its name is already here.
struct NamedProfile
{
    char  szName[24]{};
    void* pProfile = nullptr;
};

static constexpr size_t nMaxProfiles = 24;
static NamedProfile Profiles[nMaxProfiles]{};
static std::atomic<size_t> nProfiles = 0;

static void RememberProfile(const void* pName, void* pProfile)
{
    const auto* pText = StringText(pName);
    if (pText == nullptr || *pText == '\0' || pProfile == nullptr)
        return;

    const auto nCount = nProfiles.load(std::memory_order_relaxed);

    for (size_t i = 0; i < nCount; i++)
    {
        if (_stricmp(Profiles[i].szName, pText) == 0)
        {
            Profiles[i].pProfile = pProfile;
            return;
        }
    }

    if (nCount >= nMaxProfiles)
        return;

    strncpy_s(Profiles[nCount].szName, pText, _TRUNCATE);
    Profiles[nCount].pProfile = pProfile;

    // Published last, so a reader on another thread never sees a half written entry.
    nProfiles.store(nCount + 1, std::memory_order_release);
}

static void* ProfileNamed(const char* pName)
{
    if (pName == nullptr || *pName == '\0')
        return nullptr;

    const auto nCount = nProfiles.load(std::memory_order_acquire);

    for (size_t i = 0; i < nCount; i++)
    {
        if (_stricmp(Profiles[i].szName, pName) == 0)
            return Profiles[i].pProfile;
    }

    return nullptr;
}

// The name at +04h on the global profile, which the setter writes every time the level changes.
static const char* CurrentProfileName()
{
    if (ppGlobalRenderProfile == nullptr || !IsReadable(ppGlobalRenderProfile, sizeof(void*)))
        return nullptr;

    auto* pGlobal = static_cast<uint8_t*>(*ppGlobalRenderProfile);
    return pGlobal != nullptr ? StringText(pGlobal + nProfileCurrentName) : nullptr;
}

// The profile in force: the one carrying the name the engine says is in force. The d3d10 variant
// is tried as well, because the display apply appends that suffix on a d3d10 device and the engine
// then asks for both spellings.
static void* QualityProfile()
{
    const auto* pName = CurrentProfileName();
    if (pName == nullptr)
        return nullptr;

    if (auto* pExact = ProfileNamed(pName))
        return pExact;

    char szVariant[32]{};
    _snprintf_s(szVariant, _TRUNCATE, "%.20sd3d10", pName);

    return ProfileNamed(szVariant);
}

static void* __fastcall ProfileFind(void* pStore, void* pEdx, void* pName)
{
    auto* pProfile = ProfileFindHook.fastcall<void*>(pStore, pEdx, pName);

    RememberProfile(pName, pProfile);

    return pProfile;
}

// Every profile the engine has named this session, for the log.
export const char* JackalFixProfileFinds()
{
    static char szList[320]{};

    szList[0] = '\0';

    const auto nCount = nProfiles.load(std::memory_order_acquire);
    for (size_t i = 0; i < nCount; i++)
    {
        const auto nUsed = strlen(szList);
        if (nUsed + 28 >= sizeof(szList))
            break;

        _snprintf_s(szList + nUsed, sizeof(szList) - nUsed, _TRUNCATE, "%s%.20s",
            nUsed != 0 ? " " : "", Profiles[i].szName);
    }

    return szList[0] != '\0' ? szList : "none";
}

// The manager's only construction, the same anchor the mode reapply module resolves it from.
static const char* const szRenderManagerPattern =
    "6A 00 68 B8 03 00 00 E8 ? ? ? ? 83 C4 08 85 C0 74 0D 8B C8 E8 ? ? ? ? A3 ? ? ? ?";
static constexpr ptrdiff_t nRenderManagerPointer = 27;
static constexpr ptrdiff_t nRenderManagerProfileMoved = 0x31;

static void** ppRenderManager = nullptr;

/*
  Getting the renderer to look at the blocks again, which is the whole of "it only applies once you
  drop the display quality and put it back".

  Writing the block is not enough on its own and never was: the renderer takes what it needs out of
  the block when it is told the render settings have moved, and nothing was telling it. The one
  thing the display page does that this did not is raise the render settings broadcast, Dunia+3F8AB0
  on the global CRenderProfile, which walks five observer lists. Every observer that reacts to a
  quality change is on one of them, including the one that sets the render manager's mode byte, so
  raising it is exactly the round trip the settings were needing, less the round trip.

  Both are read out of the tail of Dunia+774530, the brightness apply, which ends
  MOV ECX,[<the profile>] / ADD ESP,0Ch / JMP <the broadcast>, so one pattern gives up the global
  profile's slot and the broadcast's address together.

  It is not raised where the setting changes. That can be the file watcher's thread, and the
  observers are the engine's; the request is left as a flag and spent at the head of the render
  manager's per-frame device tick, Dunia+34CA80, which is the engine's own thread at the point it
  is about to look at all of this anyway.
*/
static const char* const szRenderSettingsChangedPattern =
    "8B C8 E8 ? ? ? ? 8B 0D ? ? ? ? 83 C4 0C E9 ? ? ? ?";
static constexpr ptrdiff_t nRenderSettingsProfile = 9;  // disp32 of mov ecx,[<the global profile>]
static constexpr ptrdiff_t nRenderSettingsJump = 17;    // rel32 of the tail jmp into the broadcast

// 83 EC 20 / PUSH EBX,EBP,ESI,EDI / MOV ESI,ECX. The device tick, one stack argument.
static const char* const szDeviceTickPattern =
    "83 EC 20 53 55 56 57 8B F1 E8 ? ? ? ? 8B 44 24 34 50 8B CE E8 ? ? ? ? C6 44 24 34 01";

using RaiseRenderSettingsChanged_t = void(__fastcall*)(void* pProfile);

static RaiseRenderSettingsChanged_t RaiseRenderSettingsChanged = nullptr;

// Set where a setting moves, spent on the engine's thread.
static std::atomic<bool> bRenderSettingsMoved = false;

// Counted so the log can say which half of the live apply is not happening.
static uint32_t nReapplied = 0;
static uint32_t nBroadcasts = 0;
static uint32_t nTickWrites = 0;

// The profile's own copies of the four config blocks. Declared here because the status line reads
// them and it is written above the code that fills them.
static uint8_t* pProfileGeometry = nullptr;
static uint8_t* pProfileTerrain = nullptr;
static uint8_t* pProfileShadow = nullptr;
static uint8_t* pProfileAmbient = nullptr;

static SafetyHookInline DeviceTickHook{};

// Defined below with the rest of the block writes; wanted here because the tick is what keeps them
// in place.
static void WriteBeyondUltraBlocks();

/*
  Written every frame rather than once when the setting moves.

  The log had the mod's whole side of this working: all four blocks recognised, the values
  rewritten eight times, the broadcast raised five, and no change in the game at all. Which leaves
  one place for it to be going: the values are not still there when the renderer looks. A block is
  the engine's own object and it is free to put its own numbers back into it, and the broadcast is
  precisely the thing that would make it do so, so writing once and hoping is a race that cannot be
  won by argument.

  It is not worth winning by argument either. The whole of the write is about twenty five floats and
  three ints into four objects that are already in cache, so it costs nothing to do it on every tick
  and be certain. Whatever puts the stock numbers back has them for at most one frame, and the state
  the renderer reads is the state that was asked for.

  The broadcast is still raised when something moves, because that is what makes the renderer look
  at the blocks again rather than at whatever it took from them last time. It is raised BEFORE the
  write now, so the write is the last thing to touch the block on that tick.
*/
static void __fastcall DeviceTick(void* pManager, void* pEdx, void* pArg)
{
    if (bRenderSettingsMoved.exchange(false)
        && RaiseRenderSettingsChanged != nullptr
        && ppGlobalRenderProfile != nullptr)
    {
        if (auto* pProfile = *ppGlobalRenderProfile)
        {
            nBroadcasts++;
            RaiseRenderSettingsChanged(pProfile);
        }
    }

    WriteBeyondUltraBlocks();

    DeviceTickHook.fastcall(pManager, pEdx, pArg);
}

static void FindRenderProfile()
{
    auto find = dunia_pattern(szProfileFindPattern);
    if (!find.empty())
        ProfileFindHook = safetyhook::create_inline(find.get_first(), ProfileFind);

    auto manager = dunia_pattern(szRenderManagerPattern);
    if (!manager.empty())
        ppRenderManager = *manager.get_first<void**>(nRenderManagerPointer);

    auto changed = dunia_pattern(szRenderSettingsChangedPattern);
    if (!changed.empty())
    {
        auto* pSite = changed.get_first<uint8_t>();

        ppGlobalRenderProfile = *reinterpret_cast<void***>(pSite + nRenderSettingsProfile);

        auto* pRel = pSite + nRenderSettingsJump;
        RaiseRenderSettingsChanged = reinterpret_cast<RaiseRenderSettingsChanged_t>(
            pRel + sizeof(int32_t) + *reinterpret_cast<int32_t*>(pRel));
    }

    auto tick = dunia_pattern(szDeviceTickPattern);
    if (!tick.empty())
        DeviceTickHook = safetyhook::create_inline(tick.get_first(), DeviceTick);

}

// VirtualQuery rather than a guess, and every read below goes through it. This crashed once already
// on MOVSX ECX,[EAX] with EAX holding 1: a string whose capacity said "the text is somewhere else"
// and whose pointer was not an address, because the object it was read out of was not a profile.
static bool IsReadable(const void* pAddress, size_t nLength)
{
    if (pAddress == nullptr)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(pAddress, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;

    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;

    const auto nEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(pAddress) + nLength <= nEnd;
}

// One of the five names, or nothing. Anything that is not one of them is not a quality, which means
// what was read is not the field intended, and that is a reason to stop answering rather than to
// grey a row out on the strength of it.
static const char* KnownQuality(const char* pName)
{
    // All nine rather than the five the display page offers. Shadow can also read "off", which is
    // a level like any other and was reading as "not a quality at all", so the row it belongs was
    // left alone instead of greyed. The last three are the console and legacy entries, listed
    // because the table Dunia+3F2D30 walks has them and a name from it is a name that was meant.
    static const char* const szNames[]
    {
        "ultrahigh", "veryhigh", "high", "medium", "low", "off", "ps3", "xenon", "legacy"
    };

    if (!IsReadable(pName, nStringLocalCapacity))
        return nullptr;

    for (const auto* pKnown : szNames)
    {
        if (_stricmp(pName, pKnown) == 0)
            return pKnown;
    }

    return nullptr;
}

// The profile the engine itself handed out, and nothing inferred.
static const char* SectionQuality(uint32_t nQualityOffset)
{
    auto* pProfile = static_cast<uint8_t*>(QualityProfile());
    if (!IsReadable(pProfile, nQualityOffset + nStringCapacity + sizeof(uint32_t)))
        return nullptr;

    auto* pField = pProfile + nQualityOffset;
    const auto nCapacity = *reinterpret_cast<uint32_t*>(pField + nStringCapacity);

    return KnownQuality(nCapacity < nStringLocalCapacity
        ? reinterpret_cast<const char*>(pField + nStringBuffer)
        : *reinterpret_cast<const char**>(pField + nStringBuffer));
}

static bool SectionIsUltra(uint32_t nQualityOffset)
{
    const auto* pName = SectionQuality(nQualityOffset);
    return pName == nullptr || _stricmp(pName, szUltraHigh) == 0;
}

export bool JackalFixGeometryIsUltra() { return SectionIsUltra(nProfileGeometryQuality); }
export bool JackalFixShadowsAreUltra() { return SectionIsUltra(nProfileShadowQuality); }
export bool JackalFixTerrainIsUltra() { return SectionIsUltra(nProfileTerrainQuality); }

// The three names as they read right now, or nullptr where the profile could not be reached. Handed
// out so the menu can put them in its own log next to the rows they decide. A Beyond Ultra row
// that
// will not grey is either a quality that really says ultra or a profile that was never found, and
// from the row alone the two look identical.
export const char* JackalFixGeometryQuality() { return SectionQuality(nProfileGeometryQuality); }
export const char* JackalFixShadowQuality() { return SectionQuality(nProfileShadowQuality); }
export const char* JackalFixTerrainQuality() { return SectionQuality(nProfileTerrainQuality); }

/*
  What this module actually resolved, in one line, for the log the menu writes.

  Two sessions in a row have read all three qualities as ultrahigh with the game on medium, and
  from the name alone there is no telling which of the three ways that happens: the getter hook
  never went in, the object it caught is a preset template rather than the profile in force, or the
  profile is right and the strings really do say ultrahigh. The pointer and the hook states
  separate them.
*/
static char szRenderConfigStatus[224]{};

export const char* JackalFixRenderConfigStatus()
{
    const auto* pName = CurrentProfileName();

    _snprintf_s(szRenderConfigStatus, _TRUNCATE,
        "find %s, in force \"%.20s\", profile %p, %zu known, global %p, broadcast %p, tick %s",
        ProfileFindHook.enabled() ? "in" : "FAILED",
        pName != nullptr ? pName : "?",
        QualityProfile(), nProfiles.load(std::memory_order_relaxed),
        ppGlobalRenderProfile != nullptr ? *ppGlobalRenderProfile : nullptr,
        RaiseRenderSettingsChanged, DeviceTickHook.enabled() ? "in" : "FAILED");

    return szRenderConfigStatus;
}

static char szQualityDump[320]{};

export const char* JackalFixQualityDump()
{
    auto Name = [](uint32_t nOffset)
    {
        const auto* pName = SectionQuality(nOffset);
        return pName != nullptr ? pName : "?";
    };

    _snprintf_s(szQualityDump, _TRUNCATE,
        "vegetation %s, postfx %s, texture %s, textureres %s, water %s, depthpass %s, ambient %s",
        Name(nProfileVegetationQuality), Name(nProfilePostFxQuality),
        Name(nProfileTextureQuality), Name(nProfileTextureResolutionQuality),
        Name(nProfileWaterQuality), Name(nProfileDepthPassQuality),
        Name(nProfileAmbientQuality));

    return szQualityDump;
}

export void JackalFixTraceQualities()
{
    const auto* pGeometry = SectionQuality(nProfileGeometryQuality);
    const auto* pShadow = SectionQuality(nProfileShadowQuality);
    const auto* pTerrain = SectionQuality(nProfileTerrainQuality);

}

// What the live apply has actually managed, for the log the menu writes. A Beyond Ultra row that
// appears to do nothing is either a block that was never recognised at load, a rewrite that never
// happened, or a rewrite the renderer was never told about, and from the game those look identical.
static char szBeyondUltraStatus[224]{};

export const char* JackalFixBeyondUltraStatus()
{
    // Read straight back out of the block, because "the write happened" and "the value is there"
    // are not the same claim and only the second one matters.
    auto Field = [](const void* pBlock, uint32_t nOffset)
    {
        return pBlock != nullptr && IsReadable(pBlock, nOffset + sizeof(float))
            ? *reinterpret_cast<const float*>(static_cast<const uint8_t*>(pBlock) + nOffset)
            : -1.0f;
    };

    _snprintf_s(szBeyondUltraStatus, _TRUNCATE,
        "steps %d/%d/%s, ticks %u, block caps %.1f terrain %.1f sun %.1f, "
        "profile %p/%p/%p/%p caps %.1f terrain %.1f sun %.1f",
        nGeometryStep, nTerrainStep, bBeyondUltraShadows ? "on" : "off", nTickWrites,
        Field(pUltraGeometry, nGeometryRealTreeCapsMaxDistance),
        Field(pUltraTerrain, nTerrainDetailViewDistance),
        Field(pUltraShadow, nShadowSunShadowRange2),
        pProfileGeometry, pProfileTerrain, pProfileShadow, pProfileAmbient,
        Field(pProfileGeometry, nGeometryRealTreeCapsMaxDistance),
        Field(pProfileTerrain, nTerrainDetailViewDistance),
        Field(pProfileShadow, nShadowSunShadowRange2));

    return szBeyondUltraStatus;
}

// Writes the current settings over the blocks the loader built, then asks the renderer to pick them
// up. Every field written is one the renderer reads out of the active block, and the byte below is
// what makes it read the block again rather than whatever it took from it last time.
// A CRenderProfile is about 884h bytes, so the whole of it is walked.
static constexpr size_t nProfileScanBytes = 0x900;

// The vtable Dunia+350FE5 writes into a geometry config (MOV dword ptr [ESI],10E460AC) and the RVA
// of the broadcast, which is a known address in the same module and is what the base is worked out
// from. Geometry turning up at 660h is the proof that the object being scanned is the right one.
static constexpr ptrdiff_t nGeometryConfigVTableRva = 0xE460AC;
static constexpr ptrdiff_t nRenderSettingsChangedRva = 0x3F8AB0;
static constexpr ptrdiff_t nExpectedGeometryOffset = 0x660;

static void* pEmbeddedIn = nullptr;

static uint8_t* FindEmbedded(uint8_t* pProfile, uintptr_t nVTable)
{
    if (pProfile == nullptr || nVTable == 0)
        return nullptr;

    for (size_t nOffset = 0; nOffset + sizeof(uintptr_t) <= nProfileScanBytes; nOffset += sizeof(uintptr_t))
    {
        if (!IsReadable(pProfile + nOffset, sizeof(uintptr_t)))
            break;

        if (*reinterpret_cast<uintptr_t*>(pProfile + nOffset) == nVTable)
            return pProfile + nOffset;
    }

    return nullptr;
}

static uintptr_t VTableOf(const void* pBlock)
{
    return IsReadable(pBlock, sizeof(uintptr_t)) ? *reinterpret_cast<const uintptr_t*>(pBlock) : 0;
}

static void FindEmbeddedBlocks()
{
    auto* pProfile = ppGlobalRenderProfile != nullptr && IsReadable(ppGlobalRenderProfile, sizeof(void*))
        ? static_cast<uint8_t*>(*ppGlobalRenderProfile)
        : nullptr;

    if (pProfile == nullptr || pProfile == pEmbeddedIn)
        return;

    pEmbeddedIn = pProfile;

    pProfileGeometry = FindEmbedded(pProfile, VTableOf(pUltraGeometry));
    pProfileTerrain = FindEmbedded(pProfile, VTableOf(pUltraTerrain));
    pProfileShadow = FindEmbedded(pProfile, VTableOf(pUltraShadow));
    pProfileAmbient = FindEmbedded(pProfile, VTableOf(pHighAmbient));

    // The parsed block's vtable is the same class, so the scan above should find all four. If the
    // geometry one does not turn up, the constant out of its copy constructor is tried instead,
    // the module base coming from the broadcast, which is a resolved address in the same image.
    if (pProfileGeometry == nullptr && RaiseRenderSettingsChanged != nullptr)
    {
        const auto nBase = reinterpret_cast<uintptr_t>(RaiseRenderSettingsChanged) - nRenderSettingsChangedRva;
        pProfileGeometry = FindEmbedded(pProfile, nBase + nGeometryConfigVTableRva);
    }

}

static void WriteBeyondUltraBlocks()
{
    nTickWrites++;

    FindEmbeddedBlocks();

    /*
      Two objects per section and no more.

      Following every copy the engine made was a mistake that cost a crash. The log has why: the
      geometry block was copied to 18100AA8, then 181009C8, then 19966CA8, a new address each time.
      Those are temporaries, made and read and destroyed, and a list of them is a list of freed
      pointers
      being written to on every tick, which walks the heap until something falls over.

      The two that are real are the parsed block, so a quality applied later is copied with these
      values already in it, and the one embedded in the global profile, which is what the renderer
      reads. Both are owned for the life of the process.
    */
    if (pUltraGeometry != nullptr)
        ApplyGeometry(static_cast<uint8_t*>(const_cast<void*>(pUltraGeometry)));

    if (pUltraTerrain != nullptr)
        ApplyTerrain(static_cast<uint8_t*>(const_cast<void*>(pUltraTerrain)));

    if (pUltraShadow != nullptr)
        ApplyShadow(static_cast<uint8_t*>(const_cast<void*>(pUltraShadow)));

    if (pHighAmbient != nullptr)
        ApplyAmbient(static_cast<uint8_t*>(const_cast<void*>(pHighAmbient)));

    if (pProfileGeometry != nullptr)
        ApplyGeometry(pProfileGeometry);

    if (pProfileTerrain != nullptr)
        ApplyTerrain(pProfileTerrain);

    if (pProfileShadow != nullptr)
        ApplyShadow(pProfileShadow);

    if (pProfileAmbient != nullptr)
        ApplyAmbient(pProfileAmbient);
}

static void ReapplyBeyondUltra()
{
    nReapplied++;

    WriteBeyondUltraBlocks();

    // The blocks are only half of it. The other half is telling the renderer they moved, which is
    // the broadcast the display page raises, and which is spent on the engine's own thread.
    bRenderSettingsMoved = true;

    if (ppRenderManager == nullptr || !IsReadable(ppRenderManager, sizeof(void*)))
        return;

    auto* pManager = static_cast<uint8_t*>(*ppRenderManager);
    if (!IsReadable(pManager, nRenderManagerProfileMoved + 1))
        return;

    // Written from the file watcher's thread and read from the engine's, which is one byte either
    // way and the same shape the engine uses on itself.
    *(pManager + nRenderManagerProfileMoved) = 1;

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
            TakeSettings();

            // No early out: the shadow scale fix has no setting, so the hooks always go in.

            // CFloatProperty::Serialise. esi = object + descriptor->offset, then a call through
            // the visitor's float slot at +0x10C.
            auto floatPattern = dunia_pattern("56 8B C1 8B 70 0C 03 74 24 08 8B 4C 24 0C 8B 11 83 C0 04 56 50 8B 82 0C 01 00 00 FF D0 5E C2 08 00");

            // CIntProperty::Serialise, the same shape through the visitor's int slot at +0x114.
            auto intPattern = dunia_pattern("56 8B C1 8B 70 0C 03 74 24 08 8B 4C 24 0C 8B 11 83 C0 04 56 50 8B 82 14 01 00 00 FF D0 5E C2 08 00");

            if (floatPattern.empty() || intPattern.empty())
                return;

            SerialiseFloatHook = safetyhook::create_inline(floatPattern.get_first(), SerialiseFloat);
            SerialiseIntHook = safetyhook::create_inline(intPattern.get_first(), SerialiseInt);

            FindRenderProfile();

            // The XML is read once at startup and never again, so a change cannot go back through
            // the loader. It does not need to: the blocks the loader built are the objects the
            // renderer reads from, they are kept above, and writing to them again is the same write
            // the parse hook makes.
            JackalFix::onIniFileChange() += []()
            {
                TakeSettings();
                ReapplyBeyondUltra();
            };
        };
    }
} RenderConfig;
