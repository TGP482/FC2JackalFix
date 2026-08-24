/* Step 3 geometry and shadow numbers from Boggalog's Far Cry 2 Patched. */

module;

#include <common.hxx>
#include <cstdio>
#include <cstring>

export module renderconfig;

import common;
import dunia;
import settings;

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

// The quality id is the map key, not a field, so each section is recognised instead by one property
// whose stock value only the wanted preset carries.
static constexpr int32_t nUltraShadowMapSize = 2048;      // 16, 341, 680, 1364, 1364, 2048, 720, 720
static constexpr float fUltraMinZoomFactor = 0.10f;       // 0.70, 0.60, 0.45, 0.35, 0.225, 0.10
static constexpr int32_t nUltraAffectedByMuzzleFlash = 1; // only the ultrahigh terrain block sets it
static constexpr int32_t nHighSectorCountX = 12;          // 4, 8, 8, 12, 8

// Confirms the ultrahigh geometry block alongside MinZoomFactor.
static constexpr float fStockKillLodScale = 1.0f;

// BeyondUltraGeometry steps over the Ultra High geometry block: 0 stock, 1/2/3 twice, four and six
// times the vanilla draw distance, 4 the maximum (Boggalog's set). LOD scales and MinSize run
// backwards, so doubling a distance halves them. RealTreeNodeMinSize stays at the stock 0.002: 0
// turns the cull off and the tree renderer then drops nodes silently (map 2 foliage flicker).
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

// BeyondUltraShadows: Boggalog's Ultra High shadow set, less the two map sizes, which are fixed
// when the device builds them. Three fields move.
static constexpr float fBeyondUltraSunShadowRange0 = 8.0f;   // stock 4
static constexpr float fBeyondUltraSunShadowRange2 = 135.0f; // stock 140, which flickers
static constexpr float fBeyondUltraLeavesShadowRatio = 1.0f; // stock 0.5

// Static ambient shadow distance, the Ambient High half of the same claim.
static constexpr float fBeyondUltraMaxHemiMapDistance = 512.0f; // stock 160

// Stock values, kept because a setting can be turned off at runtime.
static constexpr float fStockSunShadowRange0 = 4.0f;
static constexpr float fStockSunShadowRange2 = 140.0f;
static constexpr float fStockLeavesShadowRatio = 0.5f;
static constexpr float fStockMaxHemiMapDistance = 160.0f;

// BeyondUltraTerrain: the same steps over the Ultra High terrain block. TerrainLodScale runs
// backwards and bottoms out at 0; the two detail distances double.
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

// Kept for the whole run, not just the parse: a setting can move while the game is running.
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

// Once per numeric attribute, after the loader wrote the value. Name and offset both checked:
// ShadowMapSize exists in Shadow and again in Ambient.
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
        // The only geometry attribute xenon and ps3 leave out; paired with KillLodScale, read first.
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

    // Fields the schema reads after the block was recognised. Only offsets this module owns, so a
    // stale block pointer cannot write into an unrelated object.
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
    // Bounded again so each table lookup is safe on its own.
    const auto nGeometry = JackalFixSettings.GetInt(PREF_BEYONDULTRAGEOMETRY);
    nGeometryStep = nGeometry < 0 ? 0 : (nGeometry > nMaxGeometryStep ? nMaxGeometryStep : nGeometry);

    const auto nTerrain = JackalFixSettings.GetInt(PREF_BEYONDULTRATERRAIN);
    nTerrainStep = nTerrain < 0 ? 0 : (nTerrain > nMaxTerrainStep ? nMaxTerrainStep : nTerrain);

    bBeyondUltraShadows = JackalFixSettings.GetInt(PREF_BEYONDULTRASHADOWS) != 0;
}

/*
  A Beyond Ultra setting does nothing unless the game runs the preset it edits, and a quality on
  CRenderProfile is a string, so the answer is a string compare. The render manager's +31h byte
  makes a change take hold; +30h is the mode change.
*/
static constexpr uint32_t nProfileTerrainQuality = 0x114;
static constexpr uint32_t nProfileGeometryQuality = 0x130;
static constexpr uint32_t nProfileShadowQuality = 0x168;

// Engine string: four bytes of header, sixteen of buffer, the length, then the capacity, which says
// whether the buffer is the text or a pointer to it.
static constexpr ptrdiff_t nStringBuffer = 0x04;
static constexpr ptrdiff_t nStringCapacity = 0x18;
static constexpr uint32_t nStringLocalCapacity = 16;

static const char* const szUltraHigh = "ultrahigh";

/*
  Applying a level in the display options makes a DIFFERENT CRenderProfile the one in force, so the
  name at +04h is read directly and the profile it names taken from the table of everything the find
  has handed out.
*/
static constexpr ptrdiff_t nProfileCurrentName = 0x04;

static const char* const szProfileFindPattern =
    "81 EC 6C 03 00 00 55 8B AC 24 74 03 00 00 56 57 55 8B F9 E8";

static SafetyHookInline ProfileFindHook{};

// The global CRenderProfile slot: the manager looks up against it and the broadcast is raised on
// it. Resolved with the broadcast further down.
static void** ppGlobalRenderProfile = nullptr;

static bool IsReadable(const void* pAddress, size_t nLength);

// Text of an engine string: the buffer is the text unless the capacity says it is a pointer.
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

// Everything the find has handed out, by name. Fixed size; a known name replaces its entry.
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

static const char* CurrentProfileName()
{
    if (ppGlobalRenderProfile == nullptr || !IsReadable(ppGlobalRenderProfile, sizeof(void*)))
        return nullptr;

    auto* pGlobal = static_cast<uint8_t*>(*ppGlobalRenderProfile);
    return pGlobal != nullptr ? StringText(pGlobal + nProfileCurrentName) : nullptr;
}

// The profile named as in force. The d3d10 variant is tried too: the display apply appends that
// suffix on a d3d10 device.
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
  Writing a block is not enough: the renderer only takes what it needs from it when the render
  settings broadcast is raised on the global profile. The brightness apply's tail gives up that
  profile's slot and the broadcast's address together. Raised on the engine's thread in the device
  tick, not where the setting changes, which can be the file watcher's.
*/
static const char* const szRenderSettingsChangedPattern =
    "8B C8 E8 ? ? ? ? 8B 0D ? ? ? ? 83 C4 0C E9 ? ? ? ?";
static constexpr ptrdiff_t nRenderSettingsProfile = 9;  // disp32 of mov ecx,[<the global profile>]
static constexpr ptrdiff_t nRenderSettingsJump = 17;    // rel32 of the tail jmp into the broadcast

// The device tick, one stack argument.
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

static void WriteBeyondUltraBlocks();

/*
  Written every frame rather than once: the broadcast makes the engine put its own numbers back, so
  writing once is a race. The broadcast is raised before the write, so the write lands last.
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

// VirtualQuery rather than a guess; every read below goes through it. Crashed once on a non-profile
// object whose string capacity said the text was elsewhere and whose pointer was not an address.
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

// One of the known names, or nothing: anything else means the field read was not a quality.
static const char* KnownQuality(const char* pName)
{
    // All nine, not the five the display page offers: shadow can read "off", and the last three are
    // the console and legacy entries from the table Dunia+3F2D30 walks.
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

// Geometry config vtable and broadcast RVAs; both moved in the GOG build.
static ptrdiff_t GeometryConfigVTableRva() { return ByBuild<ptrdiff_t>(0xE460AC, 0xDBDE38); }
static ptrdiff_t RenderSettingsChangedRva() { return ByBuild<ptrdiff_t>(0x3F8AB0, 0x3EAC70); }
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

    // The parsed block's vtable is the same class, so the scan should find all four; failing that,
    // the copy constructor's constant, off the broadcast's address.
    if (pProfileGeometry == nullptr && RaiseRenderSettingsChanged != nullptr)
    {
        const auto nBase = reinterpret_cast<uintptr_t>(RaiseRenderSettingsChanged) - RenderSettingsChangedRva();
        pProfileGeometry = FindEmbedded(pProfile, nBase + GeometryConfigVTableRva());
    }
}

static void WriteBeyondUltraBlocks()
{
    FindEmbeddedBlocks();

    /*
      Two objects per section: the parsed block, so a quality applied later is copied with these
      values, and the one embedded in the global profile, which the renderer reads.
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
      The profile's blocks are gated on its qualities; the parsed ones above are templates, only read
      through the copy made when a level is applied. The gate is the same SectionIsUltra the menu
      greys from, so row and write cannot disagree.
    */
    if (pProfileGeometry != nullptr && SectionIsUltra(nProfileGeometryQuality))
        ApplyGeometry(pProfileGeometry);

    if (pProfileTerrain != nullptr && SectionIsUltra(nProfileTerrainQuality))
        ApplyTerrain(pProfileTerrain);

    // Shadow ranges and the static ambient distance are one setting, so both follow shadow quality.
    if (pProfileShadow != nullptr && SectionIsUltra(nProfileShadowQuality))
        ApplyShadow(pProfileShadow);

    if (pProfileAmbient != nullptr && SectionIsUltra(nProfileShadowQuality))
        ApplyAmbient(pProfileAmbient);
}

static void ReapplyBeyondUltra()
{
    WriteBeyondUltraBlocks();

    // Request the broadcast; the device tick spends it on the engine's own thread.
    bRenderSettingsMoved = true;

    if (ppRenderManager == nullptr || !IsReadable(ppRenderManager, sizeof(void*)))
        return;

    auto* pManager = static_cast<uint8_t*>(*ppRenderManager);
    if (!IsReadable(pManager, nRenderManagerProfileMoved + 1))
        return;

    // Written from the file watcher's thread, read from the engine's: one byte, as the engine does.
    *(pManager + nRenderManagerProfileMoved) = 1;
}

/*
  RealTree instance budget, the other half of the map 2 foliage flicker: four jobs share one fixed
  instance array, past the cap nodes are dropped silently, and the walk order moves with the camera.

  Nine imm32 sites describe that one buffer, so one left behind is a heap overflow; every site is
  resolved and value checked before the first byte is written. Ceiling is the 16-bit index rebase,
  4 * factor * 624h <= 0FFFFh, so the factor cannot pass ten. Eight gives 12576 instances a job and
  393KB of heap in place of 49KB. The batch cap is a 12-bit field that 4 * 400h fills exactly.
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
    // Both numbers in one match: one allocation, neither meaningful alone.
    { "BF 90 18 00 00 39 BE 84 0B 00 00 73 33 8B 86 80 0B 00 00 3B C3 74 09 50 E8 ? ? ? ? 83 C4 04 "
      "53 68 80 C4 00 00 E8 ? ? ? ? 83 C4 08 89 86 80 0B 00 00", 1, 0x1890 },
    { "BF 90 18 00 00 39 BE 84 0B 00 00 73 33 8B 86 80 0B 00 00 3B C3 74 09 50 E8 ? ? ? ? 83 C4 04 "
      "53 68 80 C4 00 00 E8 ? ? ? ? 83 C4 08 89 86 80 0B 00 00", 34, 0xC480 },

    // Batch cap and instance cap of the job descriptor, anchored across the batch cap, which must
    // not move.
    { "89 4E 14 89 46 1C 8B 44 24 20 C7 06 00 04 00 00 C7 46 04 24 06 00 00 8B 8D 94 0B 00 00",
      19, 0x624 },

    // The slice handed to the job: size, then base.
    { "68 20 31 00 00 8B CB 69 C9 20 31 00 00 03 8D 80 0B 00 00 51 8B CF E8", 1, 0x3120 },
    { "68 20 31 00 00 8B CB 69 C9 20 31 00 00 03 8D 80 0B 00 00 51 8B CF E8", 9, 0x3120 },

    // The fill job; EAX tells it apart from the merge below, which is the same shape through ECX.
    { "8B 86 80 0B 00 00 68 80 C4 00 00 50 8B CF E8", 7, 0xC480 },

    // The pointer and the count in the ProcessBatches descriptor.
    { "89 9E 28 0B 00 00 89 96 34 0B 00 00 C7 86 38 0B 00 00 90 18 00 00", 18, 0x1890 },

    // The merge job's copy of the array.
    { "8B 8E 80 0B 00 00 68 80 C4 00 00 51 8B CF E8", 7, 0xC480 },

    // The merge's scalars: batch cap, then entry count. The batch cap is not written.
    { "68 00 04 00 00 8B CF E8 ? ? ? ? 68 90 18 00 00 8B CF E8", 13, 0x1890 },
};

static constexpr size_t nBudgetSites = sizeof(RealTreeBudgetSites) / sizeof(RealTreeBudgetSites[0]);

static void PatchRealTreeInstanceBudget()
{
    uint32_t* pSite[nBudgetSites]{};

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
  The spline pass multiplies the LOD threshold by LodScale where every other path divides, so
  lowering LodScale pulls the road's LOD0 radius in rather than out, and at 0.0 nothing is LOD0. The
  mesh fallback runs LOD0 -> LOD1 only, so a segment that never built LOD0 is not drawn at all,
  leaving terrain and *_Roadside textures on screen.

  MULSS to DIVSS is one opcode byte: identity at the stock 1.0, threshold +INF at 0.0 so every
  segment holds LOD0. A spline with LOD0Distance 0 divides to NaN and falls to LOD1, as before.
*/

// Anchored across the whole decision at Dunia+3C1A49, not on the MULSS, which is everywhere.
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

            // Fixes rather than settings, so they go in ahead of the property hooks' early out. The
            // budget must land before CRealTreeRenderer is constructed.
            PatchRoadLodPolarity();
            PatchRealTreeInstanceBudget();

            // No early out: the shadow scale fix has no setting, so the hooks always go in.

            // Both numeric serialisers are shared by every config class, so every numeric attribute
            // of every <quality> block arrives with its descriptor and nothing depends on patch.dat.
            // CFloatProperty::Serialise, calling through the visitor's float slot at +0x10C.
            auto floatPattern = dunia_pattern("56 8B C1 8B 70 0C 03 74 24 08 8B 4C 24 0C 8B 11 83 C0 04 56 50 8B 82 0C 01 00 00 FF D0 5E C2 08 00");

            // CIntProperty::Serialise, the same shape through the visitor's int slot at +0x114.
            auto intPattern = dunia_pattern("56 8B C1 8B 70 0C 03 74 24 08 8B 4C 24 0C 8B 11 83 C0 04 56 50 8B 82 14 01 00 00 FF D0 5E C2 08 00");

            if (floatPattern.empty() || intPattern.empty())
                return;

            SerialiseFloatHook = safetyhook::create_inline(floatPattern.get_first(), SerialiseFloat);
            SerialiseIntHook = safetyhook::create_inline(intPattern.get_first(), SerialiseInt);

            FindRenderProfile();

            // The XML is read once at startup; the blocks the loader built are what the renderer
            // reads, and they are kept above.
            JackalFix::onIniFileChange() += []()
            {
                TakeSettings();
                ReapplyBeyondUltra();
            };
        };
    }
} RenderConfig;
