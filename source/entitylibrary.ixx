/*
  Four entity library bug fixes from Boggalog's Far Cry 2 Patched, in code.

  Credit to Boggalog for identifying all four and for the values used here. His versions are data
  edits to generated/entitylibrarypatchoverride.fcb inside patch.dat:

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

  Two of them are worth a sentence on what the numbers mean. The MAC-10's muzzle stim is the noise
  the shot broadcasts to AI, and every other level-8 weapon in the game - AK47, G3KA4, M16, SPAS12,
  USAS12, Star45, Uzi - carries radius 75. The MAC-10 alone has 3, which is inside the muzzle, so
  nobody ever hears it: a transcription slip rather than a design choice. And the assassination
  target's 4/4/4 field-of-view multipliers are the set the game gives snipers and mortar crews;
  0.75/1/1.25 is what every ordinary merc gets.

  ---------------------------------------------------------------------------------------------
  Why this is one module and one hook

  All four are values on named prototypes, and two of them cannot be recognised by value alone:
  fMoveSpeedFactor is 1.0 on 134 other weapon entries, and the 4/4/4/2/0.5/6/0.15 multiplier set
  is shared by 28 enemy archetypes - which is precisely the bug. So the fix needs to know which
  prototype it is looking at, and that rules out intercepting the property system, which sees
  offsets and values but never names.

  It also rules out the obvious hook. Every one of these six floats goes through the generic float
  property descriptor vtable 0x10E98014, whose serialiser is 0x10957940 - the same function
  renderconfig.ixx already hooks. Hooking it twice is not safe.

  The way through is that Far Cry 2 does not bake the entity library into per-prototype structs at
  load. It parses each .fcb into a DOM of nodes and keeps it for the session; components are read
  out of that DOM on demand when an entity spawns. So editing the DOM is editing the archive, just
  after it has been read rather than before.

  FUN_105492E0 is where to do it. It is the per-file prototype indexer, called once per entity
  library .fcb immediately after the file's root node joins the library:

    if (!initialised) { g_hidName = { "hidName", 0xB9295CC7 }; }
    root = *ppRoot
    for each child of root:                         ; one per EntityLibrary
      for each child of that:                       ; one per EntityPrototype
        ent = proto->GetChildByName({"Entity", 0x0984415E})
        if (ent && ent->GetProperty(&g_hidName, &pszName))
          map[hash(pszName)] = proto

  Every prototype in the file passes through it with its hidName in hand, which is exactly the
  identity the two ambiguous fixes need.

  ---------------------------------------------------------------------------------------------
  The node model, and the mistake that is worth writing down

  There are two node classes, not one, and they are easy to conflate because they publish the same
  interface with the same vtable slot layout.

    The wrapper is a 0x10 byte heap object - vtable, refcount, inner pointer, parent - and it is
    what the library holds and what the hook is handed. Every one of its accessors begins by
    hopping to the inner node it owns:

        10233FB0  GetChildCount   MOV EAX,[ECX+8] / MOV EAX,[EAX+0xC]
        10233FC0  GetChild        MOV ECX,[ECX+8] / MOV EAX,[ECX+EAX*4+0x18] / ADD EAX,ECX
        10233FD0  GetChildByName  MOV ECX,[ECX+8] / JMP ...
        102348B0  GetProperty     MOV ECX,[ECX+8] / ... / CALL FindProperty

    The inner node is the actual DOM node, and there is a complete parallel family of accessors
    for it - 10233E10, 102349D0, 10234A00 and the rest - byte-identical to the wrapper's except
    that they operate on this directly, with no hop:

        10233E10  GetChild        MOV EAX,[ESP+4] / MOV EAX,[ECX+EAX*4+0x18] / ADD EAX,ECX

  The trap is that GetChild returns an inner node whichever class you call it on. So the root is a
  wrapper and everything below it is an inner node, and code that assumes one layout for the whole
  tree is wrong from the first child down.

  An earlier version of this module did exactly that: it read the property block itself as node+8
  for every node. That is right for the root and wrong for all of its descendants, where the
  property block is the node itself. The result was FindProperty being handed four bytes of .fcb
  payload as its cursor and dereferencing it - an access violation at Dunia+0x23433A, on the
  MOV ECX,[EAX] two instructions into the scan, with the bad cursor read from the phantom node+0x14.

  The fix is to stop reaching into node internals at all. Everything here now goes through the
  node's own vtable, read from the node at runtime, which dispatches to whichever family is
  correct for that node. FUN_105492E0 itself is the proof that this works for both: it calls
  GetChildByName and GetProperty on inner nodes and GetChild on the wrapper root, all through the
  same slot numbers.

  Slot +0xD8 is the one that matters. It hands back a pointer straight into the loaded .fcb buffer
  for a named property, whatever the property's type - the engine's "get string" is nothing more
  than this pointer handed back uninterpreted, which is why hidName and a float both come out of
  it. That pointer is the write target, and since all six edits are float to float nothing moves.

  Belt and braces: the whole walk runs inside a structured exception handler. This is a data
  patching pass over a format that is only partly understood, on a code path where being wrong
  used to mean the game does not start. If anything here is still mistaken, the fixes silently do
  not apply and the game runs.
*/

module;

#include <common.hxx>

export module entitylibrary;

import common;
import dunia;

// Only the vtable pointer can be assumed, and only because both node classes begin with one. Every
// other field differs between the wrapper and the inner node, so nothing else is declared.
struct FCBNode
{
    void** ppVTable;
};

// Vtable slots, as dword indices. Identical in both node classes.
static constexpr size_t nNodeGetChildCount = 0x14 / sizeof(void*);
static constexpr size_t nNodeGetChild = 0x18 / sizeof(void*);
static constexpr size_t nNodeGetChildByName = 0x20 / sizeof(void*);
static constexpr size_t nNodeGetProperty = 0xD8 / sizeof(void*);

// The {const char* name; uint32_t hash} pair every node accessor takes. FUN_105492E0 builds two of
// these on its own stack, which is where the Entity hash below comes from.
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

// NameHash::Set, the engine's own hash and the same call the schema builders make, so a hash
// computed here matches the archive by construction rather than by a table that could drift.
// __thiscall with three stack arguments.
using NameHash_t = void(__fastcall*)(uint32_t* pOut, void* pEdx, const char* pszName, int32_t, int32_t);

static NameHash_t NameHash = nullptr;

// Stock values, used as a guard so a prototype that has already been corrected - by a repacked
// archive, or by this module on an earlier .fcb - is left alone rather than shifted twice.
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

// The archive's hidName strings are not length prefixed once they come back as a raw pointer, so
// the comparison is bounded rather than trusting a terminator that a malformed file might omit.
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
    // Only the singleplayer prototypes, matching Boggalog's edits. The .Multi variants carry the
    // same stock MAC-10 radius but he left them alone, so they are left alone here too.
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

// Every accessor below dispatches through the node's own vtable, which is what makes the wrapper
// and the inner node interchangeable here.
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
    return reinterpret_cast<FCBNode*(__fastcall*)(FCBNode*, void*, const NameKey*)>(pNode->ppVTable[nNodeGetChildByName])(pNode, nullptr, &key);
}

// A property addressed in place in the loaded .fcb buffer, or null if this node does not carry it.
static void* GetProperty(FCBNode* pNode, const NameKey& key)
{
    void* pValue = nullptr;
    const auto pfnGet = reinterpret_cast<uint8_t(__fastcall*)(FCBNode*, void*, const NameKey*, void**)>(pNode->ppVTable[nNodeGetProperty]);
    return pfnGet(pNode, nullptr, &key, &pValue) != 0 ? pValue : nullptr;
}

// Rewrites only if the value is still the one the archive shipped. The comparison doubles as a
// type check: every key used here names a float in the schema, and a property that was not one
// would not hold the exact stock value being looked for.
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
        // The prototype holds exactly one stim at radius 3 - the muzzle one. Its other stim, the
        // impact, is at 15, so no further qualification is needed inside this prototype.
        SetFloat(pNode, KeyRadius, fStockMuzzleRadius, fFixedMuzzleRadius);
        break;

    case Prototype::M79:
        // fMoveSpeedFactor appears twice in a weapon prototype: once on the IronSight block at
        // field offset 0xD8 and once at the weapon root at 0xD4, both shipped at 1.0. Only the
        // ironsight one is meant to change, so the node is qualified by bCanIronsight, which only
        // the IronSight block carries.
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

// Components nest a handful of levels deep - component list, component, named group, array entry -
// so the walk is bounded rather than open ended. Eight is comfortably past anything in the library
// and stops a malformed file turning into a stack overflow.
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

// The same two-level walk FUN_105492E0 performs - libraries, then prototypes - reading each
// prototype's hidName the way the engine reads it. pRoot is the wrapper; everything GetChild
// hands back below it is an inner node, which is why nothing here touches a node's fields.
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
    // The original first, so the library's own name index is built from the values it expects. The
    // patch only moves floats, so nothing it does can invalidate that index.
    IndexLibraryHook.fastcall(pLibrary, pEdx, ppRoot);

    // Nothing in the walk allocates or holds anything that needs unwinding, so the handler is free
    // to swallow whatever it catches. A fix that does not apply is a bug; a fix that stops the game
    // starting is worse.
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
            auto hashPattern = dunia_pattern("8B 44 24 04 85 C0 56 8B F1 74 29 80 38 00 74 24 80 7C 24 0C 00 50 74 0E E8 ? ? ? ? 83 C4 04 89 06 5E C2 0C 00");

            // The per-file prototype indexer. Entry through the one-time init guard; the four
            // bytes of the guard's address are wildcarded, and the pattern stops before the
            // second reference to it.
            auto indexPattern = dunia_pattern("55 8B EC 83 E4 F8 83 EC 3C F6 05 ? ? ? ? 01 53 56 57 89 4C 24 1C 75 1B");

            if (hashPattern.empty() || indexPattern.empty())
                return;

            NameHash = reinterpret_cast<NameHash_t>(hashPattern.get_first());

            NameHash(&KeyMoveSpeedFactor.nHash, nullptr, KeyMoveSpeedFactor.pszName, 0, 0);
            NameHash(&KeyCanIronsight.nHash, nullptr, KeyCanIronsight.pszName, 0, 0);
            NameHash(&KeyRadius.nHash, nullptr, KeyRadius.pszName, 0, 0);
            NameHash(&KeyHeightOffset.nHash, nullptr, KeyHeightOffset.pszName, 0, 0);
            NameHash(&KeyPreCombatMultiplier.nHash, nullptr, KeyPreCombatMultiplier.pszName, 0, 0);
            NameHash(&KeyCombatMultiplier.nHash, nullptr, KeyCombatMultiplier.pszName, 0, 0);
            NameHash(&KeyPostCombatMultiplier.nHash, nullptr, KeyPostCombatMultiplier.pszName, 0, 0);

            // hidName and Entity are the two hashes FUN_105492E0 hardcodes rather than derives, so
            // hashing one of them here is a cheap check that this is the same hash the archive was
            // built with before any of the others are trusted.
            uint32_t nCheck = 0;
            NameHash(&nCheck, nullptr, KeyHidName.pszName, 0, 0);
            if (nCheck != KeyHidName.nHash)
                return;

            // The entity library is read once during startup, so nothing here is registered on the
            // ini watch - and these are bug fixes rather than preferences, so there is nothing to
            // toggle either.
            IndexLibraryHook = safetyhook::create_inline(indexPattern.get_first(), IndexLibrary);
        };
    }
} EntityLibrary;
