/*
  Four of the five entity library edits from Boggalog's Far Cry 2 Patched. Credit to him for
  identifying all five and for the values used here. His versions are data edits to
  generated/entitylibrarypatchoverride.fcb inside patch.dat:

    "Fixed the MAC-10 being silent"
        WeaponProperties.Secondary.MAC10 and .Mikes_Rusty
        MuzzleStims -> Stim (selType 1, nLevel 8): fRadius  3.0 -> 75.0

    "Fixed the player not walking slower when using ironsights with the M79"
        WeaponProperties.Secondary.M79 and .Mikes_Rusty
        IronSight: fMoveSpeedFactor  1.0 -> 0.5

    "Improved a slight misalignment of the vehicle GPS"
        gadgets.Equipped.Compass_Vehicle
        Map: fHeightOffset  0.035 -> 0.039

    "Fixed assassination targets having the same vision as snipers"
        enemy_archetypes.Missions.Assassination_Target
        FOVMultipliers: fPreCombatMultiplier 4 -> 0.75, fCombatMultiplier 4 -> 1,
                        fPostCombatMultiplier 4 -> 1.25

*/

module;

#include <common.hxx>

export module entitylibrary;

import common;
import dunia;

// Only the vtable pointer is common to both node classes; every other field differs.
struct FCBNode
{
    void** ppVTable;
};

// Vtable slots, as dword indices. Identical in both node classes.
static constexpr size_t nNodeGetChildCount = 0x14 / sizeof(void*);
static constexpr size_t nNodeGetChild = 0x18 / sizeof(void*);
static constexpr size_t nNodeGetChildByName = 0x20 / sizeof(void*);
static constexpr size_t nNodeGetProperty = 0xD8 / sizeof(void*);

// The {name, hash} pair every node accessor takes. FUN_105492E0 builds two of these on its own
// stack, which is where the Entity and hidName hashes below come from.
struct NameKey
{
    const char* pszName;
    uint32_t nHash;
};

static NameKey KeyEntity = { "Entity", 0x0984415E };
static NameKey KeyHidName = { "hidName", 0xB9295CC7 };
static NameKey KeyMoveSpeedFactor = { "fMoveSpeedFactor", 0 };
static NameKey KeyCanIronsight = { "bCanIronsight", 0 };
static NameKey KeyRadius = { "fRadius", 0 };
static NameKey KeyHeightOffset = { "fHeightOffset", 0 };
static NameKey KeyPreCombatMultiplier = { "fPreCombatMultiplier", 0 };
static NameKey KeyCombatMultiplier = { "fCombatMultiplier", 0 };
static NameKey KeyPostCombatMultiplier = { "fPostCombatMultiplier", 0 };

// NameHash::Set, the engine's own hash, so hashes computed here match the archive by construction.
// __thiscall with three stack arguments.
using NameHash_t = void(__fastcall*)(uint32_t* pOut, void* pEdx, const char* pszName, int32_t, int32_t);

static NameHash_t NameHash = nullptr;

// Stock values double as a guard against shifting an already-corrected prototype twice.
static constexpr float fStockMuzzleRadius = 3.0f;
static constexpr float fFixedMuzzleRadius = 75.0f;
static constexpr float fStockIronsightMoveSpeed = 1.0f;
static constexpr float fFixedIronsightMoveSpeed = 0.5f;
static constexpr float fStockCompassHeight = 0.035f;
static constexpr float fFixedCompassHeight = 0.039f;
static constexpr float fStockFOVMultiplier = 4.0f;
static constexpr float fFixedPreCombatMultiplier = 0.75f;
static constexpr float fFixedCombatMultiplier = 1.0f;
static constexpr float fFixedPostCombatMultiplier = 1.25f;

enum class Prototype
{
    None,
    MAC10,                // muzzle stim radius
    M79,                  // ironsight move speed
    VehicleCompass,       // GPS height offset
    AssassinationTarget,  // field of view multipliers
};

// hidName comes back as a raw pointer with no length, so the compare is bounded.
static constexpr size_t nMaxNameLength = 128;

static bool NameIs(const char* pszName, const char* pszExpected)
{
    if (pszName == nullptr)
        return false;

    auto lower = [](uint8_t c) -> uint8_t { return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + ('a' - 'A')) : c; };

    for (size_t i = 0; i < nMaxNameLength; ++i)
    {
        const auto a = static_cast<uint8_t>(pszName[i]);
        const auto b = static_cast<uint8_t>(pszExpected[i]);

        if (a == 0 || b == 0)
            return a == b;

        if (lower(a) != lower(b))
            return false;
    }

    return false;
}

static Prototype ClassifyPrototype(const char* pszHidName)
{
    // Singleplayer only, matching Boggalog's edits. The .Multi variants carry the same stock
    // MAC-10 radius but he left them alone.
    if (NameIs(pszHidName, "WeaponProperties.Secondary.MAC10")
        || NameIs(pszHidName, "WeaponProperties.Secondary.MAC10.Mikes_Rusty"))
        return Prototype::MAC10;

    if (NameIs(pszHidName, "WeaponProperties.Secondary.M79")
        || NameIs(pszHidName, "WeaponProperties.Secondary.M79.Mikes_Rusty"))
        return Prototype::M79;

    if (NameIs(pszHidName, "gadgets.Equipped.Compass_Vehicle"))
        return Prototype::VehicleCompass;

    if (NameIs(pszHidName, "enemy_archetypes.Missions.Assassination_Target"))
        return Prototype::AssassinationTarget;

    return Prototype::None;
}

static uint32_t GetChildCount(FCBNode* pNode)
{
    return reinterpret_cast<uint32_t(__fastcall*)(FCBNode*, void*)>(pNode->ppVTable[nNodeGetChildCount])(pNode, nullptr);
}

static FCBNode* GetChild(FCBNode* pNode, uint32_t nIndex)
{
    return reinterpret_cast<FCBNode*(__fastcall*)(FCBNode*, void*, uint32_t)>(pNode->ppVTable[nNodeGetChild])(pNode, nullptr, nIndex);
}

static FCBNode* GetChildByName(FCBNode* pNode, const NameKey& key)
{
    if (pNode == nullptr || pNode->ppVTable == nullptr)
        return nullptr;

    return reinterpret_cast<FCBNode*(__fastcall*)(FCBNode*, void*, const NameKey*)>(pNode->ppVTable[nNodeGetChildByName])(pNode, nullptr, &key);
}

// Pointer into the loaded .fcb buffer, or null if this node does not carry the property.
static void* GetProperty(FCBNode* pNode, const NameKey& key)
{
    void* pValue = nullptr;
    const auto pfnGet = reinterpret_cast<uint8_t(__fastcall*)(FCBNode*, void*, const NameKey*, void**)>(pNode->ppVTable[nNodeGetProperty]);
    return pfnGet(pNode, nullptr, &key, &pValue) != 0 ? pValue : nullptr;
}

// Rewrites only if the value is still the one the archive shipped, which doubles as a type check.
static bool SetFloat(FCBNode* pNode, const NameKey& key, float fStock, float fFixed)
{
    auto* pValue = static_cast<float*>(GetProperty(pNode, key));
    if (pValue == nullptr || std::fabs(*pValue - fStock) > 1e-6f)
        return false;

    *pValue = fFixed;
    return true;
}

static void ApplyToNode(Prototype ePrototype, FCBNode* pNode)
{
    switch (ePrototype)
    {
    case Prototype::MAC10:
        // Only one stim in this prototype sits at radius 3, the muzzle one. The impact stim is 15.
        SetFloat(pNode, KeyRadius, fStockMuzzleRadius, fFixedMuzzleRadius);
        break;

    case Prototype::M79:
        // fMoveSpeedFactor appears twice at 1.0: IronSight block +0xD8 and weapon root +0xD4.
        // bCanIronsight only exists on the IronSight block, so it picks the right node.
        if (GetProperty(pNode, KeyCanIronsight) != nullptr)
            SetFloat(pNode, KeyMoveSpeedFactor, fStockIronsightMoveSpeed, fFixedIronsightMoveSpeed);
        break;

    case Prototype::VehicleCompass:
        SetFloat(pNode, KeyHeightOffset, fStockCompassHeight, fFixedCompassHeight);
        break;

    case Prototype::AssassinationTarget:
        // All three land on the same FOVMultipliers node, and an archetype has exactly one.
        if (SetFloat(pNode, KeyPreCombatMultiplier, fStockFOVMultiplier, fFixedPreCombatMultiplier))
        {
            SetFloat(pNode, KeyCombatMultiplier, fStockFOVMultiplier, fFixedCombatMultiplier);
            SetFloat(pNode, KeyPostCombatMultiplier, fStockFOVMultiplier, fFixedPostCombatMultiplier);
        }
        break;

    default:
        break;
    }
}

// Components nest about four levels, so eight is past anything in the library.
static constexpr int nMaxDepth = 8;

static void ApplyToSubtree(Prototype ePrototype, FCBNode* pNode, int nDepth)
{
    if (pNode == nullptr || pNode->ppVTable == nullptr || nDepth > nMaxDepth)
        return;

    ApplyToNode(ePrototype, pNode);

    const auto nChildren = GetChildCount(pNode);
    for (uint32_t i = 0; i < nChildren; ++i)
        ApplyToSubtree(ePrototype, GetChild(pNode, i), nDepth + 1);
}

// The same two-level walk FUN_105492E0 performs: libraries, then prototypes. pRoot is the wrapper,
// everything GetChild hands back below it is an inner node.
static void PatchLibrary(FCBNode* pRoot)
{
    if (pRoot == nullptr || pRoot->ppVTable == nullptr)
        return;

    const auto nLibraries = GetChildCount(pRoot);
    for (uint32_t i = 0; i < nLibraries; ++i)
    {
        auto* pLibrary = GetChild(pRoot, i);
        if (pLibrary == nullptr || pLibrary->ppVTable == nullptr)
            continue;

        const auto nPrototypes = GetChildCount(pLibrary);
        for (uint32_t j = 0; j < nPrototypes; ++j)
        {
            auto* pPrototype = GetChild(pLibrary, j);
            if (pPrototype == nullptr || pPrototype->ppVTable == nullptr)
                continue;

            auto* pEntity = GetChildByName(pPrototype, KeyEntity);
            if (pEntity == nullptr || pEntity->ppVTable == nullptr)
                continue;

            const auto ePrototype = ClassifyPrototype(static_cast<const char*>(GetProperty(pEntity, KeyHidName)));
            if (ePrototype != Prototype::None)
                ApplyToSubtree(ePrototype, pPrototype, 0);
        }
    }
}

// __thiscall with one stack argument and a callee cleanup of four bytes.
static SafetyHookInline IndexLibraryHook{};

static void __fastcall IndexLibrary(void* pLibrary, void* pEdx, FCBNode** ppRoot)
{
    // Original first, so the name index is built before anything moves.
    IndexLibraryHook.fastcall(pLibrary, pEdx, ppRoot);

    // Nothing in the walk needs unwinding, so the handler can swallow whatever it catches.
    __try
    {
        if (ppRoot != nullptr)
            PatchLibrary(*ppRoot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

class EntityLibrary
{
public:
    EntityLibrary()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // NameHash::Set. Entry through the store of the computed hash, with the call
            // displacement wildcarded.
            auto* pHash = dunia_find("8B 44 24 04 85 C0 56 8B F1 74 29 80 38 00 74 24 80 7C 24 0C 00 50 74 0E E8 ? ? ? ? 83 C4 04 89 06 5E C2 0C 00");

            // The per-file prototype indexer. Entry through the one-time init guard, with the
            // four bytes of the guard's address wildcarded.
            auto* pIndex = dunia_find("55 8B EC 83 E4 F8 83 EC 3C F6 05 ? ? ? ? 01 53 56 57 89 4C 24 1C 75 1B");

            if (!pHash || !pIndex)
                return;

            NameHash = reinterpret_cast<NameHash_t>(pHash);

            for (auto* pKey : {
                &KeyMoveSpeedFactor, &KeyCanIronsight, &KeyRadius, &KeyHeightOffset,
                &KeyPreCombatMultiplier, &KeyCombatMultiplier, &KeyPostCombatMultiplier })
            {
                NameHash(&pKey->nHash, nullptr, pKey->pszName, 0, 0);
            }

            // hidName is one of the two hashes FUN_105492E0 hardcodes, so rehashing it checks
            // NameHash against the archive before the others are trusted.
            uint32_t nCheck = 0;
            NameHash(&nCheck, nullptr, KeyHidName.pszName, 0, 0);
            if (nCheck != KeyHidName.nHash)
                return;

            // The entity library is read once during startup, so nothing is registered on the ini
            // watch.
            IndexLibraryHook = safetyhook::create_inline(pIndex, IndexLibrary);
        };
    }
} EntityLibrary;
