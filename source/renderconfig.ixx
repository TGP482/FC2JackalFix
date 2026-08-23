module;

#include <common.hxx>
#include <cstdio>
#include <cstring>

export module renderconfig;

import common;
import dunia;
import settings;

// Each render config class (CRenderShadowConfig, CRenderGeometryConfig, CRenderTerrainConfig,
// CRenderAmbientConfig) publishes a schema of property descriptors (name, hash, field offset)
// through its vtable; the loader calls each descriptor's serialise method, which resolves
// object + descriptor->offset and hands the address to the XML reader.
//
// Only two numeric serialisers exist, float and int, shared by every config class. Hooking both
// on return covers every numeric attribute of every <quality> block with the descriptor still in
// hand, so nothing here depends on patch.dat.
//
// Schemas are registered by FUN_103F56E0 (geometry), FUN_103F7240 (terrain), FUN_103F4600
// (shadow) and FUN_103F7520 (ambient), each after the shared base FUN_103F42E0, which is why
// every derived field sits at 0x20 or above. Bools are four byte ints, so every field below is a
// float or an int32.
//
// Only the Ultra High geometry, shadow and terrain presets are touched, and Ambient High.
// The step 3 geometry and shadow numbers are Boggalog's, from Far Cry 2 Patched.
//
// One Boggalog edit is deliberately not reproduced: his <Geometry> ultrahigh TerrainLodScale="0.1"
// is inert, since CRenderGeometryConfig registers no such descriptor (the only TerrainLodScale is
// CRenderTerrainConfig+0x2C, string 0x10E5078C referenced from FUN_103F7240 alone). The real one
// is driven by BeyondUltraTerrain below.

// Property descriptor built by the schema registration functions.
struct RenderProperty
{
    void* pVTable;              // 0x00  one vtable per field type
    const char* pszName;        // 0x04  XML attribute name
    uint32_t nNameHash;         // 0x08
    uint32_t nFieldOffset;      // 0x0C  field offset inside the config object
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

// The quality id is the map key, not a field on the block, so the serialiser cannot see it. Each
// section is instead recognised by one property whose stock value is unique to the wanted preset
// ("ultrahigh" for shadow/geometry/terrain, "high" for ambient). Lower presets never carry these
// values and are left alone.
static constexpr int32_t nUltraShadowMapSize = 2048;      // 16, 341, 680, 1364, 1364, 2048, 720, 720
static constexpr float fUltraMinZoomFactor = 0.10f;       // 0.70, 0.60, 0.45, 0.35, 0.225, 0.10
static constexpr int32_t nUltraAffectedByMuzzleFlash = 1; // only the ultrahigh terrain block sets it
static constexpr int32_t nHighSectorCountX = 12;          // 4, 8, 8, 12, 8

// Stock KillLodScale of the ultrahigh geometry block, used to confirm the block alongside
// MinZoomFactor. The schema serialises KillLodScale first, so it is in place when the pair is
// tested.
static constexpr float fStockKillLodScale = 1.0f;

// BeyondUltraGeometry. Steps over the Ultra High geometry block: 0 stock, 1/2/3 twice, four and
// six times the vanilla draw distance, 4 the most the block will take (Boggalog's set).
//
// LOD scales and MinSize thresholds run backwards (lower survives further, 0 is the maximum), so
// doubling a distance halves them. Only RealTreeCapsMaxDistance and the two decal counts double.
//
// LodScale halves with the rest only because of the polarity patch further down: the spline pass
// reads it the opposite way round, so lowering it used to break road surfaces up.
//
// KillLodScale was pinned at 0.7 because scenery popped in on map 2 below that; the pop was the
// tree renderer running out of instances, raised by PatchRealTreeInstanceBudget below.
//
// No projected-size threshold reaches 0, because 0 turns the test off rather than making it
// permissive:
//
//     Dunia+3995F0  this+0x9F0/0x9F1/0x9F2 = 0.0 < node/leaf/hleaf MinSize (config+0x7C/0x74/0x78),
//                   and the job at Dunia+48B760 size-rejects only parts whose switch is set
//     Dunia+39B3D0  ANDs the three, so one zero drops the cull for trunks, leaves and hybrid leaves
//     Dunia+3BB6B0  0.0 < ClusterObjectMinSize (config+0x84), clear submits every cluster instance
//                   in the frustum at full detail
//
// Off, the culls no longer hold the tree renderer inside its instance budget and it drops nodes
// silently: the map 2 foliage flicker. So RealTreeNodeMinSize stays at the stock 0.002; leaf,
// hybrid leaf, cluster and scene halve without reaching it. Raising the budget below is also
// needed.
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
    //  CapsDist RealTrees Clusters      Lod     Kill      Leaf      HLeaf    Node   Cluster     Scene Decals PerType
    {    100.0f,     1.0f,    0.8f,    1.0f,    1.0f,    0.02f,    0.015f, 0.002f,    0.02f,    0.01f,   200,      50 }, // 0 stock
    {    200.0f,     0.5f,    0.4f,    0.5f,    0.5f,    0.01f,   0.0075f, 0.002f,    0.01f,   0.005f,   400,     100 }, // 1 twice
    {    400.0f,    0.25f,    0.2f,   0.25f,   0.25f,   0.005f,  0.00375f, 0.002f,   0.005f,  0.0025f,   800,     200 }, // 2 four times
    {    600.0f,  0.1667f, 0.1333f, 0.1667f, 0.1667f, 0.00333f,   0.0025f, 0.002f, 0.00333f, 0.00167f,  1200,     300 }, // 3 six times
    {   1000.0f,     0.1f,    0.1f,    0.0f,    0.0f,  0.0025f, 0.001875f, 0.002f,  0.0025f, 0.00125f,  2000,    1000 }, // 4 maximum
};

static constexpr int32_t nMaxGeometryStep = static_cast<int32_t>(sizeof(GeometrySteps) / sizeof(GeometrySteps[0])) - 1;

// BeyondUltraShadows. Boggalog's Ultra High shadow set, less the two map sizes: a shadow map is
// sized when the device builds it, so a setting for it could only be read at startup and never
// took effect. SunShadowFadeRange and SunShadowRange1 already carry his values, so three fields
// move.
static constexpr float fBeyondUltraSunShadowRange0 = 8.0f;   // stock 4
static constexpr float fBeyondUltraSunShadowRange2 = 135.0f; // stock 140, which flickers
static constexpr float fBeyondUltraLeavesShadowRatio = 1.0f; // stock 0.5

// Static ambient shadow distance, the Ambient High half of the same claim.
static constexpr float fBeyondUltraMaxHemiMapDistance = 512.0f; // stock 160

// Stock values, kept because a setting can be turned off at runtime, long after the XML is gone.
static constexpr float fStockSunShadowRange0 = 4.0f;
static constexpr float fStockSunShadowRange2 = 140.0f;
static constexpr float fStockLeavesShadowRatio = 0.5f;
static constexpr float fStockMaxHemiMapDistance = 160.0f;

// BeyondUltraTerrain. The same steps over the Ultra High terrain block. TerrainLodScale runs
// backwards like the geometry scales and bottoms out at 0; the two detail distances double. Step 3
// goes well past Boggalog, who only moves TerrainDetailBlendViewDistance from 64 to 128.
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

// Small object and vegetation shadows, credited to miru. Unconditional: a fix, not a setting.
static constexpr float fFixedMinSizeShadowScale = 1.0f; // stock 1.25, bar RealTree which is 1.0

static int32_t nGeometryStep = 0;
static int32_t nTerrainStep = 0;
static bool bBeyondUltraShadows = false;

// The four blocks, kept for the rest of the run, not just the parse: a setting can move while the
// game is running. (They were thread_local and only remembered when a setting asked for them.)
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

// Both directions, always: writing the stock numbers back lets the setting be turned off without a
// restart.
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

// Called once per numeric attribute, after the loader wrote the parsed value into the field. Name
// and offset are both checked: ShadowMapSize exists in the Shadow section and again in Ambient,
// where it sizes the sector ambient map.
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
        // MinZoomFactor is the only geometry attribute the xenon and ps3 blocks leave out, so pair
        // it with KillLodScale, which the schema reads first.
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

    // Fields the schema reads after the block was recognised, which the loader just put back to
    // the XML value. Only offsets this module owns are acted on, so a stale block pointer cannot
    // write into an unrelated object.
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
    // Already bounded when the ini is read; bounded again so each table lookup is safe on its own.
    const auto nGeometry = JackalFixSettings.GetInt(PREF_BEYONDULTRAGEOMETRY);
    nGeometryStep = nGeometry < 0 ? 0 : (nGeometry > nMaxGeometryStep ? nMaxGeometryStep : nGeometry);

    const auto nTerrain = JackalFixSettings.GetInt(PREF_BEYONDULTRATERRAIN);
    nTerrainStep = nTerrain < 0 ? 0 : (nTerrain > nMaxTerrainStep ? nMaxTerrainStep : nTerrain);

    bBeyondUltraShadows = JackalFixSettings.GetInt(PREF_BEYONDULTRASHADOWS) != 0;
}

/*
  Which quality levels are in force, and getting the engine to look again. A Beyond Ultra setting
  does nothing unless the game is running the preset it edits.

  A quality on CRenderProfile is a string, not an int. The schema at Dunia+3F7F00 registers
  TerrainQuality at 114h, GeometryQuality at 130h, AmbientQuality at 14Ch and ShadowQuality at 168h,
  1Ch apart (string sized), and Serialise hands object+offset to the visitor's 0DCh slot rather than
  the 114h int slot. So the field holds "ultrahigh", "veryhigh", "high", "medium" or "low", and the
  answer is a string compare.

  Making a change take hold is a second byte on the render manager. Dunia+354B30 sets +31h when it
  copies the profile in, and Dunia+33E540, the per-frame path on the engine's thread, tests that
  byte, clears it and runs the functor at +13Ch, which re-reads the profile and picks the blocks up
  again. +30h is the mode change and is deliberately left alone.
*/
static constexpr uint32_t nProfileTerrainQuality = 0x114;
static constexpr uint32_t nProfileGeometryQuality = 0x130;
static constexpr uint32_t nProfileShadowQuality = 0x168;

// The engine's string: four bytes of header, sixteen of buffer, the length, then the capacity that
// says whether the buffer is the text or a pointer to it.
static constexpr ptrdiff_t nStringBuffer = 0x04;
static constexpr ptrdiff_t nStringCapacity = 0x18;
static constexpr uint32_t nStringLocalCapacity = 16;

static const char* const szUltraHigh = "ultrahigh";

/*
  Which CRenderProfile the game is actually running. There are a dozen of them, and applying a
  level in the display options does not modify the profile in force, it makes a DIFFERENT profile
  the one in force out of the preset store, so remembering whatever a hook last handed back names
  the boot-time profile for the whole session.

  Dunia+402150 is the setter, four instructions long:

    MOV ESI,ECX
    LEA ECX,[ESI+04h]        ; the name of the profile in force
    CALL <string assign>
    ...
    CALL <the render settings broadcast>

  So the name lives at +04h as a plain std::string, written every time the level changes. That is
  read directly, and the profile it names is taken from a small table of everything the find has
  handed out.
*/
static constexpr ptrdiff_t nProfileCurrentName = 0x04;

// 81 EC 6C 03 00 00 / PUSH EBP / MOV EBP,[ESP+374h] / PUSH ESI / PUSH EDI / PUSH EBP / MOV EDI,ECX.
static const char* const szProfileFindPattern =
    "81 EC 6C 03 00 00 55 8B AC 24 74 03 00 00 56 57 55 8B F9 E8";

static SafetyHookInline ProfileFindHook{};

// The slot Dunia keeps the global CRenderProfile in: both the manager lookups are made against and
// the object the render settings broadcast is raised on. Resolved with the broadcast further down;
// declared here because the lookup hook wants it first.
static void** ppGlobalRenderProfile = nullptr;

// Declared here and defined with the rest of the guarded reads further down.
static bool IsReadable(const void* pAddress, size_t nLength);

// Text of an engine string: the buffer is the text unless the capacity says it is a pointer to it.
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

// Everything the find has handed out, by the name it was asked for. Fixed size: a dozen profiles
// and the same names asked for repeatedly, so a known name replaces its entry rather than append.
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

// The profile carrying the name the engine says is in force. The d3d10 variant is tried too: the
// display apply appends that suffix on a d3d10 device and the engine asks for both spellings.
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

// The manager's only construction, the same anchor borderless resolves it from.
static const char* const szRenderManagerPattern =
    "6A 00 68 B8 03 00 00 E8 ? ? ? ? 83 C4 08 85 C0 74 0D 8B C8 E8 ? ? ? ? A3 ? ? ? ?";
static constexpr ptrdiff_t nRenderManagerPointer = 27;
static constexpr ptrdiff_t nRenderManagerProfileMoved = 0x31;

static void** ppRenderManager = nullptr;

/*
  Getting the renderer to look at the blocks again, which is the whole of "it only applies once you
  drop the display quality and put it back".

  Writing the block is not enough: the renderer only takes what it needs out of the block when told
  the render settings moved. The display page raises the render settings broadcast, Dunia+3F8AB0 on
  the global CRenderProfile, which walks five observer lists carrying every observer that reacts to
  a quality change, including the one that sets the render manager's mode byte.

  Both are read out of the tail of Dunia+774530, the brightness apply, which ends
  MOV ECX,[<the profile>] / ADD ESP,0Ch / JMP <the broadcast>, so one pattern gives up the global
  profile's slot and the broadcast's address together.

  It is not raised where the setting changes: that can be the file watcher's thread and the
  observers are the engine's. The request is left as a flag and spent at the head of the render
  manager's per-frame device tick, Dunia+34CA80, on the engine's own thread.
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

// The profile's own copies of the four config blocks.
static uint8_t* pProfileGeometry = nullptr;
static uint8_t* pProfileTerrain = nullptr;
static uint8_t* pProfileShadow = nullptr;
static uint8_t* pProfileAmbient = nullptr;

static SafetyHookInline DeviceTickHook{};

// Defined below with the rest of the block writes; wanted here because the tick keeps them in
// place.
static void WriteBeyondUltraBlocks();

/*
  Written every frame rather than once when the setting moves: the engine is free to put its own
  numbers back into a block (the broadcast makes it do so), so writing once is a race. Twenty five
  floats and three ints into four cached objects costs nothing per tick.

  The broadcast is still raised when something moves, BEFORE the write, so the write is the last
  thing to touch the block on that tick.
*/
static void __fastcall DeviceTick(void* pManager, void* pEdx, void* pArg)
{
    if (bRenderSettingsMoved.exchange(false)
        && RaiseRenderSettingsChanged != nullptr
        && ppGlobalRenderProfile != nullptr)
    {
        if (auto* pProfile = *ppGlobalRenderProfile)
        {
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

// VirtualQuery rather than a guess; every read below goes through it. This crashed once on
// MOVSX ECX,[EAX] with EAX holding 1: a string whose capacity said the text was elsewhere and
// whose pointer was not an address, because the object was not a profile.
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

// One of the known names, or nothing: anything else means the field read was not a quality, so
// stop answering rather than grey a row out on the strength of it.
static const char* KnownQuality(const char* pName)
{
    // All nine, not the five the display page offers. Shadow can read "off", a level like any
    // other that used to read as "not a quality" and left its row ungreyed. The last three are the
    // console and legacy entries, in the table Dunia+3F2D30 walks.
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

// A CRenderProfile is about 884h bytes, so the whole of it is walked.
static constexpr size_t nProfileScanBytes = 0x900;

// The vtable Dunia+350FE5 writes into a geometry config (MOV dword ptr [ESI],10E460AC), and the
// broadcast's RVA, a known address in the same module that the base is worked out from. Geometry
// turning up at 660h proves the object being scanned is the right one.
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

    // The parsed block's vtable is the same class, so the scan should find all four. Failing that,
    // fall back to the constant from the copy constructor, based off the broadcast's address.
    if (pProfileGeometry == nullptr && RaiseRenderSettingsChanged != nullptr)
    {
        const auto nBase = reinterpret_cast<uintptr_t>(RaiseRenderSettingsChanged) - nRenderSettingsChangedRva;
        pProfileGeometry = FindEmbedded(pProfile, nBase + nGeometryConfigVTableRva);
    }
}

static void WriteBeyondUltraBlocks()
{
    FindEmbeddedBlocks();

    /*
      Two objects per section and no more: the parsed block, so a quality applied later is copied
      with these values in it, and the one embedded in the global profile, which the renderer
      reads. Both live for the process. Following every copy the engine makes would write to
      destroyed temporaries and walk the heap.
    */
    if (pUltraGeometry != nullptr)
        ApplyGeometry(static_cast<uint8_t*>(const_cast<void*>(pUltraGeometry)));

    if (pUltraTerrain != nullptr)
        ApplyTerrain(static_cast<uint8_t*>(const_cast<void*>(pUltraTerrain)));

    if (pUltraShadow != nullptr)
        ApplyShadow(static_cast<uint8_t*>(const_cast<void*>(pUltraShadow)));

    if (pHighAmbient != nullptr)
        ApplyAmbient(static_cast<uint8_t*>(const_cast<void*>(pHighAmbient)));

    /*
      The profile's own blocks are gated on its qualities; the parsed ones above are not. Those are
      templates, only read through the copy made when a level is applied, so writing them at any
      quality costs nothing and readies them for when the player raises the level. The embedded
      blocks hold whichever preset the profile names, so below ultrahigh writing Beyond Ultra into
      them applied a row the menu had greyed out.

      The gate is the same SectionIsUltra the menu greys from, so row and write cannot disagree,
      including where the profile cannot be reached and both fall open.
    */
    if (pProfileGeometry != nullptr && SectionIsUltra(nProfileGeometryQuality))
        ApplyGeometry(pProfileGeometry);

    if (pProfileTerrain != nullptr && SectionIsUltra(nProfileTerrainQuality))
        ApplyTerrain(pProfileTerrain);

    // Shadow ranges and the static ambient distance are one setting, so both follow the shadow
    // quality rather than the ambient one.
    if (pProfileShadow != nullptr && SectionIsUltra(nProfileShadowQuality))
        ApplyShadow(pProfileShadow);

    if (pProfileAmbient != nullptr && SectionIsUltra(nProfileShadowQuality))
        ApplyAmbient(pProfileAmbient);
}

static void ReapplyBeyondUltra()
{
    WriteBeyondUltraBlocks();

    // Telling the renderer they moved: the broadcast the display page raises, spent on the
    // engine's own thread.
    bRenderSettingsMoved = true;

    if (ppRenderManager == nullptr || !IsReadable(ppRenderManager, sizeof(void*)))
        return;

    auto* pManager = static_cast<uint8_t*>(*ppRenderManager);
    if (!IsReadable(pManager, nRenderManagerProfileMoved + 1))
        return;

    // Written from the file watcher's thread, read from the engine's: one byte either way, the
    // same shape the engine uses on itself.
    *(pManager + nRenderManagerProfileMoved) = 1;
}

/*
  RealTree instance budget, the other half of the map 2 foliage flicker.

  CRealTreeRenderer cuts the visible set into four jobs, each with a fixed instance array.
  Dunia+397420 writes cap 624h into every job descriptor; the array is allocated once in the
  constructor at Dunia+399F70, 1890h entries of 8 bytes. Dunia+48B5D0 refuses a batch when either
  count reaches its cap and returns -1, and Dunia+48B760 drops the node with no assert and no
  growth. Which nodes arrive past the cap follows the walk order, which moves with the camera, so
  the loss reads as blinking rather than a distance limit.

  Measured on map 2 at BeyondUltraGeometry 3: instances saturated at 1572 of 1572 with 230 nodes a
  frame refused, batches peaked at 190 of 1024; the same view off the setting peaked at 800 and lost
  nothing. RealTreesLodScale fills it, holding trees at near LODs ten times further out, but
  clamping that back gives up the setting to work around a fixed array.

  Nine imm32 sites describe the one buffer:

      1039A47B  MOV EDI,1890h            entries, stored to +0B84h and +0B88h
      1039A49C  PUSH 0C480h              malloc, entries * 8
      103974B9  MOV [ESI+4],624h         per job cap, and the multiplier the rebase uses
      1039757F  PUSH 3120h               per job slice size
      10397586  IMUL ECX,ECX,3120h       per job slice stride
      103976E9  PUSH 0C480h              whole array, the fill job's input
      1039C03D  MOV [ESI+0B38h],1890h    whole array, in the ProcessBatches descriptor
      1039BD25  PUSH 0C480h              whole array, for the merge job
      1039BD73  PUSH 1890h               the merge's own entry count

  Four size the memory and five say how big it is, so one left behind is a heap overflow rather than
  a smaller improvement. Every site is resolved and value checked before the first byte is written.

  The ceiling is the rebase at the tail of Dunia+48BFE0, which turns each job's local indices into
  global ones:

      IMUL  EAX,[EBP+50h]     ; jobIndex * instanceCap
      MOVZX ECX,AX            ; ...as sixteen bits
      ADD   word ptr [EAX],CX

  Instance links and the head and tail of every batch header are sixteen bit fields with 0FFFFh as
  the empty marker, so 4 * factor * 624h <= 0FFFFh and the factor cannot pass ten. Eight gives 12576
  instances a job and 393KB of heap in place of 49KB; the same view then peaks at 1861 and loses
  nothing, so the stock cap was around 20% short.

  The batch cap is left alone. Its index is a twelve bit field masked 0FFFh in both Dunia+48B5D0 and
  the merge, and 4 * 400h fills it exactly. It was never the one running out.
*/
static constexpr uint32_t nInstanceBudgetFactor = 8;

struct BudgetSite
{
    const char* pszPattern;
    ptrdiff_t nOffset;   // of the imm32 inside the match
    uint32_t nStock;
};

static constexpr BudgetSite RealTreeBudgetSites[]
{
    // MOV EDI,1890h / CMP [ESI+0B84h],EDI / JNC / ... / PUSH 0C480h / CALL <malloc>. Both numbers
    // are in one match because they are one allocation and neither is meaningful alone.
    { "BF 90 18 00 00 39 BE 84 0B 00 00 73 33 8B 86 80 0B 00 00 3B C3 74 09 50 E8 ? ? ? ? 83 C4 04 "
      "53 68 80 C4 00 00 E8 ? ? ? ? 83 C4 08 89 86 80 0B 00 00", 1, 0x1890 },
    { "BF 90 18 00 00 39 BE 84 0B 00 00 73 33 8B 86 80 0B 00 00 3B C3 74 09 50 E8 ? ? ? ? 83 C4 04 "
      "53 68 80 C4 00 00 E8 ? ? ? ? 83 C4 08 89 86 80 0B 00 00", 34, 0xC480 },

    // MOV [ESI],400h / MOV [ESI+4],624h, the batch cap and the instance cap of the job descriptor.
    // Anchored across the batch cap, which is the one that must not move.
    { "89 4E 14 89 46 1C 8B 44 24 20 C7 06 00 04 00 00 C7 46 04 24 06 00 00 8B 8D 94 0B 00 00",
      19, 0x624 },

    // PUSH 3120h / MOV ECX,EBX / IMUL ECX,ECX,3120h / ADD ECX,[EBP+0B80h], the slice handed to the
    // job: size, then base.
    { "68 20 31 00 00 8B CB 69 C9 20 31 00 00 03 8D 80 0B 00 00 51 8B CF E8", 1, 0x3120 },
    { "68 20 31 00 00 8B CB 69 C9 20 31 00 00 03 8D 80 0B 00 00 51 8B CF E8", 9, 0x3120 },

    // MOV EAX,[ESI+0B80h] / PUSH 0C480h / PUSH EAX, the fill job. EAX and PUSH 50 tell it apart
    // from the merge below, which is the same shape through ECX.
    { "8B 86 80 0B 00 00 68 80 C4 00 00 50 8B CF E8", 7, 0xC480 },

    // MOV [ESI+0B34h],EDX / MOV [ESI+0B38h],1890h, the pointer and the count in the ProcessBatches
    // descriptor.
    { "89 9E 28 0B 00 00 89 96 34 0B 00 00 C7 86 38 0B 00 00 90 18 00 00", 18, 0x1890 },

    // MOV ECX,[ESI+0B80h] / PUSH 0C480h / PUSH ECX, the merge job's copy of the array.
    { "8B 8E 80 0B 00 00 68 80 C4 00 00 51 8B CF E8", 7, 0xC480 },

    // PUSH 400h / ... / PUSH 1890h, the merge's scalars: batch cap, then entry count. The batch cap
    // is in the match for the same reason as above and is not written.
    { "68 00 04 00 00 8B CF E8 ? ? ? ? 68 90 18 00 00 8B CF E8", 13, 0x1890 },
};

static constexpr size_t nBudgetSites = sizeof(RealTreeBudgetSites) / sizeof(RealTreeBudgetSites[0]);

static void PatchRealTreeInstanceBudget()
{
    uint32_t* pSite[nBudgetSites]{};

    // Every site resolved and read back before anything is written. Half a patch is a buffer that
    // something still believes is the old size, and that is worse than no patch at all.
    for (size_t i = 0; i < nBudgetSites; i++)
    {
        auto pattern = dunia_pattern(RealTreeBudgetSites[i].pszPattern);
        if (pattern.empty())
            return;

        auto* pValue = pattern.get_first<uint32_t>(RealTreeBudgetSites[i].nOffset);
        if (pValue == nullptr || *pValue != RealTreeBudgetSites[i].nStock)
            return;

        pSite[i] = pValue;
    }

    for (size_t i = 0; i < nBudgetSites; i++)
        injector::WriteMemory<uint32_t>(pSite[i], RealTreeBudgetSites[i].nStock * nInstanceBudgetFactor, true);
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

/*
  Roads and LodScale, which the spline pass reads the wrong way round.

  Roads are CSceneSplinePrimitive ribbons drawn by their own pass at Dunia+3C18A0, and each segment
  chooses between exactly two meshes at Dunia+3C1A5B:

      MOVSS  XMM1, [EBP+44h]      ; LOD0Distance, 92 for every road in spline_inventory.xml
      MULSS  XMM1, [EBX+48h]      ; * the frame's composed LOD scale
      MOVAPS XMM2, XMM1
      MULSS  XMM2, XMM1           ; squared
      COMISS XMM2, XMM0           ; against squared distance to the segment
      JBE    3C1A6E               ; threshold <= distance, stay on LOD1

  Every other LOD path scales the distance and leaves the threshold alone. Scene objects at
  Dunia+3C20E0 test scale * distance < lodDistance[i], so a switch sits at threshold / scale, and
  clusters at Dunia+3BB6B0 cache (threshold / scale) squared to reach the same place. The spline
  pass scales the threshold instead, putting its switch at threshold * scale. Lowering LodScale
  moves every other switch out and pulls the road's LOD0 radius in: 92m at 1.0, 46m at 0.5, and at
  0.0 no segment is ever LOD0 at any distance.

  Losing LOD0 does not just coarsen the ribbon. Both meshes are built on demand, and the fallback at
  Dunia+3C1A9F runs one way, LOD0 -> LOD1: take lodMesh[chosen], if null queue a build, if still
  null and chosen is 0 take lodMesh[1], if still null skip the segment. A segment that has been near
  the camera since it came into range has only ever had LOD0 built, so asking it for LOD1 asks for a
  mesh that does not exist, and with no LOD1 -> LOD0 fallback it is not drawn at all. What is left
  on screen is the terrain it was covering, painted with the *_Roadside textures.

  MULSS to DIVSS is one opcode byte and puts the pass on the polarity everything else already uses.
  It is identity at the stock 1.0, and at 0.0 the threshold becomes +INF so every segment holds
  LOD0, which is what the rest of the maximum step asks for anyway. The one-way fallback then runs
  in the safe direction: an unbuilt LOD0 drops to LOD1 and the road is coarse for a frame instead of
  missing. A spline carrying LOD0Distance 0 divides to a NaN, COMISS reports unordered and JBE is
  taken, which is the LOD1 the multiply already gave it.
*/

// From MOV EDI,1 at Dunia+3C1A49 through MOV EAX,[ESI+EDI*4+4] at Dunia+3C1A6E. Anchored across the
// whole decision rather than on the MULSS, which as F3 0F 59 4B 48 occurs all over the image.
static const char* const szSplineLodPattern =
    "BF 01 00 00 00 F3 0F 11 44 24 3C 74 18 F3 0F 10 4D 44 F3 0F 59 4B 48 0F 28 D1 F3 0F 59 D1 "
    "0F 2F D0 76 02 33 FF 8B 44 BE 04";

// The 59 of MULSS XMM1,[EBX+48h], measured from the start of the pattern above.
static constexpr ptrdiff_t nSplineScaleOpcode = 0x14;

static constexpr uint8_t nOpcodeDivss = 0x5E;

static void PatchRoadLodPolarity()
{
    auto pattern = dunia_pattern(szSplineLodPattern);
    if (pattern.empty())
        return;

    auto* pOpcode = pattern.get_first<uint8_t>(nSplineScaleOpcode);
    injector::WriteMemory<uint8_t>(pOpcode, nOpcodeDivss, true);
}

class RenderConfig
{
public:
    RenderConfig()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            TakeSettings();

            // Fixes rather than settings, and they share nothing with the property hooks, so they
            // go in ahead of their early out. The budget has to be in before CRealTreeRenderer is
            // constructed, which is a device away, and Dunia has only just been mapped.
            PatchRoadLodPolarity();
            PatchRealTreeInstanceBudget();

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
