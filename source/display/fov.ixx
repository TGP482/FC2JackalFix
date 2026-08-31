/* Based on FoxAhead's Far Cry 2 Multi Fixer: https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer */

module;

#include <common.hxx>
#include <atomic>

export module fov;

import common;
import dunia;
import settings;

static constexpr float fPi = 3.14159265f;

static float fFieldOfView = 75.0f;
static float fViewmodelFieldOfView = 75.0f;
static float fIronsightFieldOfView = 0.0f;
static float fVehicleFieldOfView = 0.0f;

// Cutscenes, ladders and gliders break above the stock 75: reframed shots, first person body edges.
// 45 is the floor FieldOfView itself clamps to.
static constexpr float fFieldOfViewFloor = 45.0f;

// Hang gliders. Measured before the engine's Hor+ stretch, so this renders as 91.31 on 16:9.
static constexpr float fGliderFieldOfViewMax = 75.0f;

// Cutscenes and ladders. 59.85 comes back out at 75 after the stretch. One ceiling for both: a
// ladder mount enters a scene context, so separate ceilings pull the view twice.
static constexpr float fNarrowFieldOfViewMax = 59.85f;

static float fGliderFieldOfView = fGliderFieldOfViewMax;
static float fNarrowFieldOfView = fNarrowFieldOfViewMax;

static float DegreesToRadians(float fDegrees)
{
    return fDegrees * (fPi / 180.0f);
}

// Narrows only. Spelled out rather than std::min, which Windows.h's min macro breaks.
static void NarrowFieldOfView(float* pFov, float fCap)
{
    if (*pFov > fCap)
        *pFov = fCap;
}

// Partway to the cap, for a clamp that has to arrive over several frames.
static void BlendFieldOfView(float* pFov, float fCap, float fWeight)
{
    if (*pFov > fCap)
        *pFov += (fCap - *pFov) * fWeight;
}

// CCameraBoneComponent::Update gets the secondary base, +4, so fFOV (+0x70, radians) is at +0x6C
// and the Cinematic flag (+0x90) at +0x8C.
static constexpr uintptr_t nBoneCameraFieldOfView = 0x6C;

// The camera state CCameraPawnComponent::Update fills in: +0x28 world FOV, +0x30 model FOV, both
// radians, both untransformed here. The 0x84 block is copy on write, so nothing may key on it.
static constexpr uintptr_t nCameraStateFieldOfView = 0x28;
static constexpr uintptr_t nCameraStateViewmodelFieldOfView = 0x30;

// Active, CCameraComponent +0x10, reached from the secondary base the update is handed.
static constexpr uintptr_t nCameraActive = 0x0C;

// Frame delta: the update's first argument.
static constexpr uintptr_t nCameraDeltaTime = 0x08;

// Roughly the mount animation length, so the clamp arrives over it instead of snapping.
static constexpr float fNarrowBlendSeconds = 0.35f;

// A frame delta past this is a load or a hitch; stepping the blend by it would snap.
static constexpr float fNarrowBlendMaxStep = 0.1f;

// First person camera update only, so no atomics. Keyed on the component, never on the pooled
// camera state, where the owner test only passes on the frames the buffer is reused.
static float fNarrowBlend = 0.0f;
static uintptr_t nNarrowBlendCamera = 0;

// Which context armed the blend, held while it decays.
static uint32_t nNarrowBlendContext = 0;

// Held, not re-read: the context clearing at scene end would swap ceilings mid blend and pop.
static float fNarrowBlendCap = fNarrowFieldOfViewMax;

// CPawnBeautifierComponent. ContextBeautifier: the instance picked for the pawn's current context.
static constexpr uintptr_t nContextBeautifier = 0x28;

// TypeBeautifier: the player/AI half, which separates the local player from every other pawn.
static constexpr uintptr_t nTypeBeautifier = 0x2C;

// Context names the situation, type names whose pawn, so one classifier covers both. DominoPlayer,
// CinematicFirst and FirstNoControl are the non-gameplay first person contexts.
enum BeautifierKind : uint32_t
{
    KIND_OTHER = 0,
    KIND_LADDER,
    KIND_CUTSCENE,
    KIND_PLAYER,
};

// What the first person camera is currently in, if anything.
enum NarrowContext : uint32_t
{
    NARROW_NONE = 0,
    NARROW_LADDER,
    NARROW_CUTSCENE,
};

// CVehicle's FOV block: fFOVTransitionTime +0x230, fFOVAngle +0x234, archFOVCurveName +0x238.
static constexpr uintptr_t nVehicleName = 0x00;
static constexpr uintptr_t nVehicleFieldOfViewAngle = 0x234;
static constexpr uint32_t nVehicleNameParaglider = 0x7B2D589C;

// The player's pawn only, so an AI on a ladder cannot narrow the view. Stamped with an expiry, not
// latched: quickloading out of a scene destroys the pawn with no transition to clear it.
static std::atomic<uint32_t> nNarrowContext = NARROW_NONE;
static std::atomic<uint32_t> nNarrowStamp = 0;

// Long enough to ride out a stutter, short enough that a load releases the clamp unseen.
static constexpr uint32_t nNarrowFreshnessMs = 250;

// Whether the near pass follows the camera this frame. Published by the pawn camera hook.
static std::atomic<bool> bNearPassFollowsCamera = false;

// CPawnFOV+0x34, the vehicle channel weight, sampled once a frame: zero on foot, one in a seat.
static std::atomic<float> fVehicleFovWeight = 0.0f;

static bool NarrowStateIsStale()
{
    return GetTickCount() - nNarrowStamp.load(std::memory_order_relaxed) > nNarrowFreshnessMs;
}

// Naming a beautifier instance: GetClassInfo on primary vtable slot 1, descriptor's first field the
// name literal. Every pointer range and page checked, so a broken convention cannot fault.

static uintptr_t nDuniaBase = 0;
static uintptr_t nDuniaEnd = 0;

static bool InDunia(const void* p)
{
    auto n = reinterpret_cast<uintptr_t>(p);
    return nDuniaBase != 0 && n >= nDuniaBase && n < nDuniaEnd;
}

static bool IsReadableStruct(uintptr_t nAddress, size_t nSize)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (nAddress == 0 || VirtualQuery((const void*)nAddress, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;

    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;

    auto nEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return nAddress + nSize <= nEnd;
}

struct DuniaClassInfo
{
    const char* szName;
    uint32_t nDepth;
};

using tGetClassInfo = const DuniaClassInfo* (__thiscall*)(const void*);

static const char* ClassName(uintptr_t nObject)
{
    if (!IsReadableStruct(nObject, sizeof(void*)))
        return "";

    auto pVtable = *(void***)nObject;
    if (!InDunia(pVtable) || !IsReadableStruct((uintptr_t)pVtable, 2 * sizeof(void*)) || !InDunia(pVtable[1]))
        return "";

    auto pClassInfo = ((tGetClassInfo)pVtable[1])((const void*)nObject);
    if (!InDunia(pClassInfo) || !IsReadableStruct((uintptr_t)pClassInfo, sizeof(DuniaClassInfo)))
        return "";

    return InDunia(pClassInfo->szName) ? pClassInfo->szName : "";
}

// Sixteen is more beautifier classes than a pawn cycles through in a session.
struct BeautifierClass
{
    uintptr_t nVtable;
    uint32_t nKind;
};

static BeautifierClass BeautifierClasses[16]{};
static std::mutex BeautifierClassMutex;

static uint32_t KindOf(uintptr_t nInstance)
{
    if (nInstance == 0 || !IsReadableStruct(nInstance, sizeof(void*)))
        return KIND_OTHER;

    // A freed instance reads garbage here; refusing to cache it keeps it off a real class.
    auto nVtable = *(uintptr_t*)nInstance;
    if (!InDunia((const void*)nVtable))
        return KIND_OTHER;

    std::scoped_lock lock(BeautifierClassMutex);

    for (const auto& entry : BeautifierClasses)
    {
        if (entry.nVtable == nVtable)
            return entry.nKind;
    }

    auto szClass = ClassName(nInstance);

    uint32_t nKind = KIND_OTHER;
    if (std::strcmp(szClass, "CPawnBeautifierLadder") == 0)
        nKind = KIND_LADDER;
    else if (std::strcmp(szClass, "CPawnBeautifierDominoPlayer") == 0
          || std::strcmp(szClass, "CPawnBeautifierCinematicFirst") == 0
          || std::strcmp(szClass, "CPawnBeautifierFirstNoControl") == 0)
        nKind = KIND_CUTSCENE;
    else if (std::strcmp(szClass, "CPawnBeautifierPlayer") == 0)
        nKind = KIND_PLAYER;

    for (auto& entry : BeautifierClasses)
    {
        if (entry.nVtable == 0)
        {
            entry.nVtable = nVtable;
            entry.nKind = nKind;
            break;
        }
    }

    return nKind;
}

// Scale in tangent space to match the game's own widescreen scaling.
static float ScaleFov(float fovRad, float scale)
{
    if (scale == 1.0f)
        return fovRad;

    return 2.0f * std::atan(std::tan(fovRad * 0.5f) * scale);
}


// The engine's own widescreen stretch factor, the constant at 0x10EAF2F8.
static constexpr float fWidescreenStretch = 0.75f;

// The near pass gets the camera's FOV, ironsight zoom included. Draw at ViewmodelFieldOfView, and
// follow the camera once it is the narrower, which keeps the sights lined up with the world.

// How far the sights are up, sampled once a frame: zero down, one fully raised.
static std::atomic<float> fIronsightBlend = 0.0f;

// How far the clamp has pulled the world in, in tangent space, and the ceiling it heads for; one is
// zero when the other is. Scaling the near pass by it holds weapon and world at one tangent ratio.
static std::atomic<float> fNarrowTangentScale = 1.0f;
static std::atomic<float> fNarrowViewmodelCeiling = 0.0f;

static void ClearNarrowViewmodel()
{
    fNarrowTangentScale.store(1.0f, std::memory_order_relaxed);
    fNarrowViewmodelCeiling.store(0.0f, std::memory_order_relaxed);
}

static float ViewmodelScaleFor(float fovRad, float aspect)
{
    auto fCameraTan = std::tan(fovRad * 0.5f);
    if (fCameraTan <= 0.0f)
        return 1.0f;

    auto fViewmodelTan = std::tan(fViewmodelFieldOfView * (fPi / 360.0f))
                       * fNarrowTangentScale.load(std::memory_order_relaxed)
                       * fWidescreenStretch * aspect;

    // The camera state is clamped before the widescreen stretch, so the ceiling takes it here.
    auto fCeiling = fNarrowViewmodelCeiling.load(std::memory_order_relaxed);
    if (fCeiling > 0.0f)
    {
        auto fCeilingTan = std::tan(fCeiling * 0.5f) * fWidescreenStretch * aspect;
        if (fViewmodelTan > fCeilingTan)
            fViewmodelTan = fCeilingTan;
    }

    auto fScale = fViewmodelTan / fCameraTan;

    if (fScale <= 1.0f)
        return fScale;

    // A Viewmodel FOV above Field of View is legitimate, so no flat ceiling of one, except with
    // the sights up, where widening the near pass back out would hold the gun off the eye.
    return fIronsightBlend.load(std::memory_order_relaxed) > 0.0f ? 1.0f : fScale;
}

// Only rewrite unmagnified sights, magnified optics use the same property.
static constexpr float fMagnifiedOpticCutoff = 40.0f;

// The weapon property object's ironsight FOV. Radians, unlike the vehicle's degrees, so the
// magnified-optic cutoff is compared after converting.
static constexpr uintptr_t nWeaponIronsightFieldOfView = 0xE4;

// Past the FLD and the FSTP that copy it into the channel, six bytes and three.
static constexpr size_t nIronsightFovCopied = 9;

static float RadiansToDegrees(float fRadians)
{
    return fRadians * (180.0f / fPi);
}

// The first person camera's own FOV, on the primary base. Not 6Ch, the model's FOV: writing that
// ties Field of View to Viewmodel FOV.
static constexpr uintptr_t nPawnCameraFieldOfView = 0x70;

// The pawn's FOV channels, blended from a CPawnFOV struct rather than read where they are set. Two
// 0x1C records: A at 0x08 ironsight, B at 0x24 base, each +0x14 target and +0x18 curve.
static constexpr uintptr_t nPawnFovIronsightValue = 0x1C;   // radians

// Record A's blend weight, zero to one. Read, never written.
static constexpr uintptr_t nPawnFovIronsightWeight = 0x18;
static constexpr uintptr_t nPawnFovBaseValue      = 0x38;   // radians
static constexpr uintptr_t nPawnFovBaseWeight     = 0x34;
static constexpr uintptr_t nPawnFovBaseArmed      = 0x29;
static constexpr size_t    nPawnFovSize           = 0x60;

// No path to the struct from a global, so it is remembered where the engine hands it over and
// vtable checked before reuse: the owning pawn does not survive a load.
static std::atomic<uintptr_t> nPawnFieldOfView = 0;
static std::atomic<uintptr_t> nPawnFieldOfViewVTable = 0;

static void RememberPawnFieldOfView(uintptr_t nStruct)
{
    // Called once a frame from the blend, so a repeat pointer costs a compare, not a page query.
    if (nStruct == 0 || nStruct == nPawnFieldOfView.load(std::memory_order_relaxed))
        return;

    if (!IsReadableStruct(nStruct, nPawnFovSize))
        return;

    nPawnFieldOfView.store(nStruct, std::memory_order_relaxed);

    // Learned from the first struct handed over, so no pattern is spent finding the class vtable.
    if (nPawnFieldOfViewVTable.load(std::memory_order_relaxed) == 0)
        nPawnFieldOfViewVTable.store(*(uintptr_t*)nStruct, std::memory_order_relaxed);
}

// The struct, or nothing once the owning pawn has gone.
static uintptr_t LivePawnFieldOfView()
{
    auto nStruct = nPawnFieldOfView.load(std::memory_order_relaxed);
    auto nVTable = nPawnFieldOfViewVTable.load(std::memory_order_relaxed);

    if (nStruct == 0 || nVTable == 0 || !IsReadableStruct(nStruct, nPawnFovSize))
        return 0;

    return *(uintptr_t*)nStruct == nVTable ? nStruct : 0;
}

// Whether the player's own weapon is a magnified optic. An already pushed value reads back as a
// scope, so the push below cannot judge it; the setup hook decides and leaves the answer here.
static std::atomic<bool> bIronsightIsMagnifiedOptic = false;

// ApplySeatFieldOfView is the only place the paraglider is identified, so the answer is kept.
static std::atomic<bool> bSeatIsParaglider = false;

// Re-pushes both channels from the settings. Armed flags only where already set, so a player on
// foot is never handed a vehicle's FOV; targets are written either way.
static void PushPawnFieldOfView()
{
    auto nStruct = LivePawnFieldOfView();
    if (nStruct == 0)
        return;

    if (fIronsightFieldOfView > 0.0f && !bIronsightIsMagnifiedOptic.load(std::memory_order_relaxed))
    {
        // Radians: the channel copies the weapon's radian field. Armed flag left as the engine
        // has it, or a channel nothing asked for starts.
        *(float*)(nStruct + nPawnFovIronsightValue) = DegreesToRadians(fIronsightFieldOfView);
    }

    if (*(uint8_t*)(nStruct + nPawnFovBaseArmed) == 0)
        return;

    // The paraglider takes the glider ceiling, never VehicleFieldOfView: this push lands after
    // ApplySeatFieldOfView's clamp. Written outright, since narrowing could not raise it back.
    if (bSeatIsParaglider.load(std::memory_order_relaxed) &&
        fVehicleFovWeight.load(std::memory_order_relaxed) > 0.0f)
    {
        *(float*)(nStruct + nPawnFovBaseValue) = DegreesToRadians(fGliderFieldOfView);
        *(uint8_t*)(nStruct + nPawnFovBaseArmed) = 1;
        return;
    }

    if (fVehicleFieldOfView > 0.0f)
    {
        *(float*)(nStruct + nPawnFovBaseValue) = DegreesToRadians(fVehicleFieldOfView);
        *(uint8_t*)(nStruct + nPawnFovBaseArmed) = 1;
    }
}

// Muzzle particles: weapon in the near pass, flash and smoke in the world one, so the two come
// apart once the fields of view differ. Mask read from the owner: 1 on the weapon, 0 on every AI.
static constexpr uintptr_t nGraphicFirstPersonMask = 0x12C;

using tSetFirstPersonLayerMask = void (__thiscall*)(void*, uint32_t);

static tSetFirstPersonLayerMask SetFirstPersonLayerMask = nullptr;

// EAX the resolved CParticlesSystemInstance, one instruction before the Start that consumes it;
// EBX the owner's CGraphicComponent.
static void MuzzleFirstPersonMask(SafetyHookContext& regs)
{
    if (regs.eax == 0)
        return;

    auto pGraphic = (void*)regs.ebx;
    if (!IsReadableStruct((uintptr_t)pGraphic, nGraphicFirstPersonMask + sizeof(uint32_t)))
        return;

    auto nMask = *(uint32_t*)((uintptr_t)pGraphic + nGraphicFirstPersonMask);

    // A zero is what the emitters already carry, so third person and AI are left alone.
    if (nMask == 0)
        return;

    SetFirstPersonLayerMask((void*)regs.eax, nMask);
}

// The machete blade mid swing: the layer comes off the weapon while the blade still draws at the
// eye. Only timing tells a swing from a swap, so the clear is withheld until show, pose or this.
static constexpr float fSwingMaxSeconds = 1.6f;

// selWeaponClass at +0x40; class 0 is every HandToHand archetype. Not sufficient alone, but it
// keeps out every hide made with a gun in hand.
static constexpr uintptr_t nWeaponClass = 0x40;
static constexpr uint32_t nWeaponClassMelee = 0;

static std::atomic<bool> bMeleeInHand = false;

static void RememberMeleeWeapon(uint32_t nClass)
{
    bMeleeInHand.store(nClass == nWeaponClassMelee, std::memory_order_relaxed);
}

static std::atomic<uintptr_t> nLayerWithheldFrom = 0;
static std::atomic<uint32_t> nLayerWithheldBits = 0;

// Gameplay seconds, stepped by the camera update's delta: on GetTickCount a pause aged the hold.
static std::atomic<float> fLayerWithheldFor = 0.0f;

// The mask reaches node+0x90 only through the push, which walks it out to each object entry.
// Writing +0x12C alone leaves the nodes carrying 1 and first person geometry over the world.
using tPushFirstPersonLayer = void (__fastcall*)(uintptr_t);

static tPushFirstPersonLayer PushFirstPersonLayer = nullptr;

// The player's first person set, kept in one pass for a scripted scene, which hides exactly one
// component and leaves the rest winning on near pass depth. The push entry is a census of the set.
static constexpr size_t nFirstPersonSetMax = 32;

struct FirstPersonComponent
{
    uintptr_t nComponent;

    // The vtable from census time, when the component was certainly live. Compared, never called.
    uintptr_t nVTable;

    uint32_t nSuppressed;

    // Set when the engine hid or showed this component during the scene; handing our bits back then
    // writes over a decision it made after the copy was taken.
    bool bEngineTouched;
};

static FirstPersonComponent FirstPersonSet[nFirstPersonSetMax]{};
static std::mutex FirstPersonSetMutex;
static std::atomic<bool> bFirstPersonSetSuppressed = false;

// Our own pushes re-enter the census hook, which would take the mutex we are already holding.
static thread_local bool bWritingLayer = false;

// A recorded pointer outlives its entity, and page checks still pass on the reused address. The
// validator must not execute anything, so the census vtable is compared rather than ClassName.
static bool IsLiveEntry(const FirstPersonComponent& entry)
{
    if (entry.nComponent == 0 || entry.nVTable == 0)
        return false;

    if (!IsReadableStruct(entry.nComponent, nGraphicFirstPersonMask + sizeof(uint32_t)))
        return false;

    return *(uintptr_t*)entry.nComponent == entry.nVTable;
}

// The layer push entry, ECX the component.
static void RememberFirstPersonComponent(SafetyHookContext& regs)
{
    if (bWritingLayer)
        return;

    // Live and readable without a page query; this entry is hot and a VirtualQuery per call is not.
    auto nComponent = regs.ecx;
    if (nComponent == 0)
        return;

    auto nVTable = *(uintptr_t*)nComponent;
    if (!InDunia((const void*)nVTable))
        return;

    // Only the ones carrying a mask: an AI's components push a zero through the same call.
    if (*(uint32_t*)(nComponent + nGraphicFirstPersonMask) == 0)
        return;

    std::scoped_lock lock(FirstPersonSetMutex);

    for (const auto& entry : FirstPersonSet)
    {
        if (entry.nComponent == nComponent)
            return;
    }

    for (auto& entry : FirstPersonSet)
    {
        if (entry.nComponent == 0)
        {
            entry.nComponent = nComponent;
            entry.nVTable = nVTable;
            entry.nSuppressed = 0;
            entry.bEngineTouched = false;
            return;
        }
    }
}

// Whether the engine has already taken one of the set into the world pass. With the set whole the
// right number of writes is none.
static bool FirstPersonSetIsSplit()
{
    auto bAnyHidden = false;
    auto bAnyShown = false;

    for (const auto& entry : FirstPersonSet)
    {
        if (!IsLiveEntry(entry))
            continue;

        if (*(uint32_t*)(entry.nComponent + nGraphicFirstPersonMask) == 0)
            bAnyHidden = true;
        else
            bAnyShown = true;
    }

    return bAnyHidden && bAnyShown;
}

static void SuppressFirstPersonSet()
{
    if (bFirstPersonSetSuppressed.load(std::memory_order_relaxed))
        return;

    std::scoped_lock lock(FirstPersonSetMutex);

    if (!FirstPersonSetIsSplit())
        return;

    bFirstPersonSetSuppressed.store(true, std::memory_order_relaxed);

    bWritingLayer = true;

    for (auto& entry : FirstPersonSet)
    {
        if (entry.nComponent == 0)
            continue;

        // Freed or reused between scenes: dropped rather than written.
        if (!IsLiveEntry(entry))
        {
            entry.nComponent = 0;
            entry.nVTable = 0;
            entry.nSuppressed = 0;
            continue;
        }

        auto pMask = (uint32_t*)(entry.nComponent + nGraphicFirstPersonMask);

        // Already in the world pass: the one the engine hid. Nothing to save.
        if (*pMask == 0)
            continue;

        entry.nSuppressed = *pMask;
        entry.bEngineTouched = false;
        *pMask = 0;

        if (PushFirstPersonLayer != nullptr)
            PushFirstPersonLayer(entry.nComponent);
    }

    bWritingLayer = false;
}

static void RestoreFirstPersonSet()
{
    if (!bFirstPersonSetSuppressed.exchange(false, std::memory_order_relaxed))
        return;

    std::scoped_lock lock(FirstPersonSetMutex);

    bWritingLayer = true;

    for (auto& entry : FirstPersonSet)
    {
        auto nSuppressed = entry.nSuppressed;
        auto bTouched = entry.bEngineTouched;

        entry.nSuppressed = 0;
        entry.bEngineTouched = false;

        if (entry.nComponent == 0 || nSuppressed == 0 || bTouched)
            continue;

        if (!IsLiveEntry(entry))
        {
            entry.nComponent = 0;
            entry.nVTable = 0;
            continue;
        }

        *(uint32_t*)(entry.nComponent + nGraphicFirstPersonMask) |= nSuppressed;

        if (PushFirstPersonLayer != nullptr)
            PushFirstPersonLayer(entry.nComponent);
    }

    bWritingLayer = false;
}

static void ForgetFirstPersonSet()
{
    std::scoped_lock lock(FirstPersonSetMutex);

    for (auto& entry : FirstPersonSet)
    {
        entry.nComponent = 0;
        entry.nVTable = 0;
        entry.nSuppressed = 0;
        entry.bEngineTouched = false;
    }
}

// Once a frame on the gameplay thread. Keyed on the context, not the hide: either arrives first.
static void UpdateFirstPersonSetForScene()
{
    // A stale narrow state is a load or a teardown: drop the table, the census fills it again.
    if (NarrowStateIsStale())
    {
        RestoreFirstPersonSet();
        ForgetFirstPersonSet();
        return;
    }

    // A seat is the engine's own split, already answered by the near pass fade; suppressing there
    // fights it. Boats and gliders reach a scene context while mounting.
    auto bScene = nNarrowContext.load(std::memory_order_relaxed) == NARROW_CUTSCENE
        && fVehicleFovWeight.load(std::memory_order_relaxed) <= 0.0f;

    if (bScene)
        SuppressFirstPersonSet();
    else
        RestoreFirstPersonSet();
}

// Dropped rather than written when the component no longer carries the withheld bits.
static void ReleaseWithheldLayer()
{
    auto nComponent = nLayerWithheldFrom.exchange(0, std::memory_order_relaxed);
    auto nBits = nLayerWithheldBits.load(std::memory_order_relaxed);

    if (nComponent == 0 || nBits == 0)
        return;

    if (!IsReadableStruct(nComponent, nGraphicFirstPersonMask + sizeof(uint32_t)))
        return;

    auto pMask = (uint32_t*)(nComponent + nGraphicFirstPersonMask);

    if ((*pMask & nBits) != nBits)
        return;

    *pMask &= ~nBits;

    if (PushFirstPersonLayer != nullptr)
        PushFirstPersonLayer(nComponent);
}

// A seat, a ladder or a cutscene, each taking the weapon out of the player's hands through the call
// a swing uses. Checked from the near pass hook too: the camera blend stops once a seat is taken.
static bool InScriptedPose()
{
    if (fVehicleFovWeight.load(std::memory_order_relaxed) > 0.0f)
        return true;

    if (NarrowStateIsStale())
        return false;

    auto nContext = nNarrowContext.load(std::memory_order_relaxed);

    return nContext != NARROW_NONE;
}

// The deadline, on the gameplay thread the delta comes from. A swing is back in about 1.3 seconds,
// so this only fires for a hide whose show never came.
static void StepWithheldLayer(float fDelta)
{
    if (nLayerWithheldFrom.load(std::memory_order_acquire) == 0)
        return;

    auto fHeldFor = fLayerWithheldFor.load(std::memory_order_relaxed) + fDelta;

    fLayerWithheldFor.store(fHeldFor, std::memory_order_relaxed);

    if (fHeldFor > fSwingMaxSeconds)
        ReleaseWithheldLayer();
}

static void ReleaseWithheldLayerIfStale()
{
    if (nLayerWithheldFrom.load(std::memory_order_acquire) == 0)
        return;

    if (InScriptedPose())
        ReleaseWithheldLayer();
}

// CGraphicComponent's enable/disable form, __thiscall(mask, bEnable): mask at [ESP+4], flag at
// [ESP+8].
static void HoldFirstPersonLayer(SafetyHookContext& regs)
{
    auto nComponent = regs.ecx;
    if (nComponent == 0)
        return;

    auto pMask = (uint32_t*)(regs.esp + 4);
    auto bEnable = *(uint8_t*)(regs.esp + 8) != 0;

    // While suppressed, the engine's own change outranks our copy: mark it, never hand it back.
    if (bFirstPersonSetSuppressed.load(std::memory_order_relaxed))
    {
        std::scoped_lock lock(FirstPersonSetMutex);

        for (auto& entry : FirstPersonSet)
        {
            if (entry.nComponent == nComponent)
            {
                entry.bEngineTouched = true;
                break;
            }
        }
    }

    if (bEnable)
    {
        // The same component coming back is the swing ending; it already holds the value.
        if (nLayerWithheldFrom.load(std::memory_order_relaxed) == nComponent)
        {
            nLayerWithheldFrom.store(0, std::memory_order_relaxed);
            return;
        }

        // A different component means the hide was a swap.
        ReleaseWithheldLayer();
        return;
    }

    if (!bMeleeInHand.load(std::memory_order_relaxed))
        return;

    if (InScriptedPose())
        return;

    if (!IsReadableStruct(nComponent, nGraphicFirstPersonMask + sizeof(uint32_t)))
        return;

    // Only a component already carrying the layer: an AI's machete never has it.
    auto nWithheld = *(uint32_t*)(nComponent + nGraphicFirstPersonMask) & *pMask;
    if (nWithheld == 0)
        return;

    // A second hide with no show between them: the first was a swap as well.
    if (nLayerWithheldFrom.load(std::memory_order_relaxed) != 0)
        ReleaseWithheldLayer();

    // Elapsed and bits before the component: the render thread keys on the component, so publishing
    // it first lets a stale elapsed time release the swing on its first frame.
    fLayerWithheldFor.store(0.0f, std::memory_order_relaxed);
    nLayerWithheldBits.store(nWithheld, std::memory_order_relaxed);
    nLayerWithheldFrom.store(nComponent, std::memory_order_release);

    *pMask = 0;
}

// Map markers are 3D entities on CCompassObjectives. In a vehicle the map moves to the world pass
// while the markers stay in the near pass, which viewmodel scaling then misaligns.
static constexpr uintptr_t nCompassInVehicleOffset = 0x28;

static std::atomic<bool> bMarkersInVehicle = false;
static std::atomic<uint32_t> nMarkersStamp = 0;

// Marker placement stops when the map closes, so the flag is only valid briefly after an update.
static constexpr uint32_t nMarkerFreshnessMs = 250;

static bool MapIsInVehicle()
{
    if (!bMarkersInVehicle.load(std::memory_order_relaxed))
        return false;

    return GetTickCount() - nMarkersStamp.load(std::memory_order_relaxed) <= nMarkerFreshnessMs;
}

// The near pass FOV; the projection hook and the frustum constants must agree, so worked out once.
static float NearPassFieldOfView(float fCameraFov, float fAspect)
{
    if (MapIsInVehicle())
        return fCameraFov;

    // The near pass copies the world's constant block and rewrites only the matrix, so a different
    // field of view draws against the wrong frustum and slides. A cutscene needs none of it.
    if (bNearPassFollowsCamera.load(std::memory_order_relaxed))
        return fCameraFov;

    auto fNear = ScaleFov(fCameraFov, ViewmodelScaleFor(fCameraFov, fAspect));

    // In a seat the near pass follows the camera, or the hands are not where the wheel is. Faded
    // over the seat's own fFOVTransitionTime rather than switched, in tangent space.
    auto fSeat = std::clamp(fVehicleFovWeight.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (fSeat <= 0.0f)
        return fNear;

    // Seated, the camera's own number rather than the arithmetic that arrives at it: the tan/atan
    // round trip lands a few ulps away.
    if (fSeat >= 1.0f)
        return fCameraFov;

    auto fNearTan = std::tan(fNear * 0.5f);
    auto fCameraTan = std::tan(fCameraFov * 0.5f);

    return 2.0f * std::atan(fNearTan + (fCameraTan - fNearTan) * fSeat);
}

// No near pass depth reconstruction: rebuilding the depth matrix at +0x260 from +0x210 draws the
// arms over the cab in every vehicle and over the frame in the glider.

// Shared by the two inlined copies of the seat FOV push. The paraglider is clamped to the glider
// ceiling; every other vehicle takes the setting outright.
static void ApplySeatFieldOfView(uintptr_t nVehicle)
{
    auto bParaglider = *(uint32_t*)(nVehicle + nVehicleName) == nVehicleNameParaglider;

    bSeatIsParaglider.store(bParaglider, std::memory_order_relaxed);

    if (bParaglider)
    {
        NarrowFieldOfView((float*)(nVehicle + nVehicleFieldOfViewAngle), fGliderFieldOfView);
        return;
    }

    if (fVehicleFieldOfView > 0.0f)
        *(float*)(nVehicle + nVehicleFieldOfViewAngle) = fVehicleFieldOfView;
}

// Both seat hooks: EAX the pawn's FOV channels, ESI the vehicle.
static void OnSeatFieldOfView(SafetyHookContext& regs)
{
    RememberPawnFieldOfView(regs.eax);
    ApplySeatFieldOfView(regs.esi);
}

// Near pass routing is the engine's own: renderContext+0x24 & node+0x90 (CGraphicComponent+0x12C).
// Not promoted at the routing tests: the node cannot name its owner, and world geometry follows.

class FieldOfView
{
public:
    FieldOfView()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Image bounds for the beautifier name walk: everything past the instance is inside
            // Dunia, so a stray pointer fails closed.
            nDuniaBase = reinterpret_cast<uintptr_t>(hDunia);
            if (nDuniaBase != 0)
            {
                auto pDos = (const IMAGE_DOS_HEADER*)nDuniaBase;
                auto pNt = (const IMAGE_NT_HEADERS*)(nDuniaBase + pDos->e_lfanew);
                nDuniaEnd = nDuniaBase + pNt->OptionalHeader.SizeOfImage;
            }

            static auto FieldOfViewCB = []()
            {
                fFieldOfView = JackalFixSettings.GetFloat(PREF_FIELDOFVIEW);
                fViewmodelFieldOfView = JackalFixSettings.GetFloat(PREF_VIEWMODELFIELDOFVIEW);
                fIronsightFieldOfView = JackalFixSettings.GetFloat(PREF_IRONSIGHTFIELDOFVIEW);
                fVehicleFieldOfView = JackalFixSettings.GetFloat(PREF_VEHICLEFIELDOFVIEW);
                fGliderFieldOfView = std::clamp(fFieldOfView, fFieldOfViewFloor, fGliderFieldOfViewMax);
                fNarrowFieldOfView = std::clamp(fFieldOfView, fFieldOfViewFloor, fNarrowFieldOfViewMax);

                // Straight into the pawn, not waiting for the next weapon setup or seat.
                PushPawnFieldOfView();
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

            // Near pass for the weapon and arms, its own projection. ESI the camera, aspect +0x18.
            auto viewmodelPattern = dunia_pattern("D9 86 28 02 00 00 D9 1C 24 E8 ? ? ? ? D9 45 14");
            if (!viewmodelPattern.empty())
            {
                static auto ViewmodelFovHook = safetyhook::create_mid(viewmodelPattern.get_first(9), [](SafetyHookContext& regs)
                {
                    ReleaseWithheldLayerIfStale();

                    // Map open in a vehicle: NearPassFieldOfView hands the camera FOV back, so the
                    // markers keep their alignment.
                    auto pFov = (float*)regs.esp;
                    auto fNear = NearPassFieldOfView(*pFov, *(float*)(regs.esi + 0x18));

                    *pFov = fNear;
                });
            }

            // Off the anim notify spawner's own call: the target has no prologue to anchor on.
            auto firstPersonMaskPattern = dunia_pattern("8B BF 2C 01 00 00 57 8D 4C 24 20 E8 ? ? ? ? 8B C8 E8");
            if (!firstPersonMaskPattern.empty())
            {
                auto pCall = firstPersonMaskPattern.get_first<uint8_t>(0x12);
                SetFirstPersonLayerMask = (tSetFirstPersonLayerMask)(pCall + 5 + *(int32_t*)(pCall + 1));
            }

            // The two weapon effect spawners, differing in stack layout. Hooked on the MOV ECX,EAX
            // before any emitter runs, EAX the system, EBX the component; the second needs a tail.
            auto muzzleFirstPattern = dunia_pattern("84 C0 75 59 8D 54 24 20 52 8D 4C 24 1C E8 ? ? ? ? 8B C8 E8 ? ? ? ? 8D 4C 24 18 E8 ? ? ? ? 8B C8 E8");
            auto muzzleSecondPattern = dunia_pattern("84 C0 75 59 8D 44 24 10 50 8D 4C 24 10 E8 ? ? ? ? 8B C8 E8 ? ? ? ? 8D 4C 24 0C E8 ? ? ? ? 8B C8 E8 ? ? ? ? 8B 97 A0 00 00 00");

            if (SetFirstPersonLayerMask != nullptr && !muzzleFirstPattern.empty())
            {
                static auto MuzzleFirstHook = safetyhook::create_mid(muzzleFirstPattern.get_first(0x22), MuzzleFirstPersonMask);
            }

            if (SetFirstPersonLayerMask != nullptr && !muzzleSecondPattern.empty())
            {
                static auto MuzzleSecondHook = safetyhook::create_mid(muzzleSecondPattern.get_first(0x22), MuzzleFirstPersonMask);
            }

            // The push half of the layer, for handing a withheld clear back to the engine.
            auto layerPushPattern = dunia_pattern("56 8B F1 83 BE C0 01 00 00 00 57 75 25 8B 46 08");
            if (!layerPushPattern.empty())
            {
                PushFirstPersonLayer = (tPushFirstPersonLayer)layerPushPattern.get_first<void>();

                // The same entry, as a census of every component carrying a near pass mask.
                static auto LayerCensusHook = safetyhook::create_mid(layerPushPattern.get_first(), RememberFirstPersonComponent);
            }

            // The melee weapon's layer, held through the swing. ESI the component, bEnable [ESP+8].
            auto layerTogglePattern = dunia_pattern("80 7C 24 08 00 56 8B F1 74 1E 8B 8E 30 01 00 00");
            if (!layerTogglePattern.empty())
            {
                static auto LayerToggleHook = safetyhook::create_mid(layerTogglePattern.get_first(), HoldFirstPersonLayer);
            }

            // Marker placement, ECX the owning CCompassObjectives; bInVehicle at +0x28 read here.
            auto compassMarkerPattern = dunia_pattern("55 8B EC 83 E4 F0 81 EC 84 00 00 00 53 56 57 8B 7D 10 8B 77 0C 8B D9");
            if (!compassMarkerPattern.empty())
            {
                static auto CompassMarkerHook = safetyhook::create_mid(compassMarkerPattern.get_first(), [](SafetyHookContext& regs)
                {
                    bMarkersInVehicle.store(*(bool*)(regs.ecx + nCompassInVehicleOffset), std::memory_order_relaxed);
                    nMarkersStamp.store(GetTickCount(), std::memory_order_relaxed);
                });
            }

            // The Widescreen option's blend weight can stick at zero after a toggle, leaving FOV
            // unwidened. Force the branch and pin the weight to 1.0; recomputed every frame.
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

            // Cutscenes. CCameraBoneComponent::Update stores fFOV into both world (+0x28) and model
            // (+0x30) FOV, so rewriting it ahead of the first FLD covers both. +0x8C is Cinematic.
            auto cinematicFovPattern = dunia_pattern("D9 47 6C 8B 54 24 40 D9 5E 28 D9 47 6C 52 D9 5E 30");
            if (!cinematicFovPattern.empty())
            {
                static auto CinematicFovHook = safetyhook::create_mid(cinematicFovPattern.get_first(), [](SafetyHookContext& regs)
                {
                    NarrowFieldOfView((float*)(regs.edi + nBoneCameraFieldOfView), DegreesToRadians(fNarrowFieldOfView));
                });
            }

            // Ladders and first person scenes carry no FOV to intercept, so the beautifier context
            // is read at CPawnBeautifierComponent::Update's entry, one frame old. ECX-4 is primary.
            auto beautifierUpdatePattern = dunia_pattern("55 56 8B E9 57 8B 7D 04 8B 77 0C 83 47 08 01 8B CE E8 ? ? ? ? 83 3D ? ? ? ? 00 75 07");
            if (!beautifierUpdatePattern.empty())
            {
                static auto BeautifierUpdateHook = safetyhook::create_mid(beautifierUpdatePattern.get_first(), [](SafetyHookContext& regs)
                {
                    auto nComponent = regs.ecx - 4;

                    // Every AI updates through here, so the type half is checked first.
                    if (KindOf(*(uintptr_t*)(nComponent + nTypeBeautifier)) != KIND_PLAYER)
                        return;

                    auto nContext = NARROW_NONE;
                    switch (KindOf(*(uintptr_t*)(nComponent + nContextBeautifier)))
                    {
                    case KIND_LADDER:   nContext = NARROW_LADDER; break;
                    case KIND_CUTSCENE: nContext = NARROW_CUTSCENE; break;
                    default: break;
                    }

                    nNarrowContext.store(nContext, std::memory_order_relaxed);
                    nNarrowStamp.store(GetTickCount(), std::memory_order_relaxed);
                });
            }

            // Field of View, live, at the head of the frame's FOV blend: ESI+70h is rebuilt every
            // frame, the setter only seeding it. Anchored on the load: ESI camera, EAX CPawnFOV.
            auto cameraFovBlendPattern = dunia_pattern("F3 0F 10 4E 70 F3 0F 10 47 14 0F 57 D2 F3 0F 5C C1 F3 0F 59 47 10");
            if (!cameraFovBlendPattern.empty())
            {
                static auto CameraFovBlendHook = safetyhook::create_mid(cameraFovBlendPattern.get_first(), [](SafetyHookContext& regs)
                {
                    // Before the MOVSS this stands on, so the blend uses it on this frame.
                    *(float*)(regs.esi + nPawnCameraFieldOfView) = DegreesToRadians(fFieldOfView);

                    RememberPawnFieldOfView(regs.eax);
                    fIronsightBlend.store(*(float*)(regs.eax + nPawnFovIronsightWeight), std::memory_order_relaxed);
                    fVehicleFovWeight.store(*(float*)(regs.eax + nPawnFovBaseWeight), std::memory_order_relaxed);

                    // The swing deadline: the one hook running every frame on the gameplay thread.
                    ReleaseWithheldLayerIfStale();

                    // Same reason, and required: moving the set allocates through a first-use
                    // branch only reached from the gameplay tick; from the near pass hook it faulted.
                    UpdateFirstPersonSetForScene();
                });
            }

            // CCameraPawnComponent::Update for first person, both FOV stores landed and before the
            // widescreen Hor+ transform. Past the FSTP; the blend is tied to the camera it raised.
            auto pawnCameraFovPattern = dunia_pattern("D9 47 6C 8B 4C 24 3C D9 5E 30 8A 9E 80 00 00 00");
            if (!pawnCameraFovPattern.empty())
            {
                static auto NarrowFovHook = safetyhook::create_mid(pawnCameraFovPattern.get_first(10), [](SafetyHookContext& regs)
                {
                    // Only the camera actually driving the view. An inactive one still updates.
                    if (*(uint8_t*)(regs.edi + nCameraActive) == 0)
                        return;

                    // On the update's own delta, so a paused game does not age the hold.
                    auto fFrame = *(float*)(regs.ebp + nCameraDeltaTime);

                    StepWithheldLayer(std::clamp(fFrame, 0.0f, fNarrowBlendMaxStep));

                    // The pawn that owned the state is gone: drop the clamp outright.
                    if (NarrowStateIsStale())
                    {
                        fNarrowBlend = 0.0f;
                        nNarrowBlendCamera = 0;
                        nNarrowBlendContext = NARROW_NONE;
                        ClearNarrowViewmodel();
                        bNearPassFollowsCamera.store(false, std::memory_order_relaxed);
                        return;
                    }

                    auto nContext = nNarrowContext.load(std::memory_order_relaxed);
                    auto bNarrow = nContext != NARROW_NONE;

                    // A different active camera: start again rather than carry a weight across
                    // views, and never hold a pointer that is not coming back.
                    if (nNarrowBlendCamera != regs.edi)
                    {
                        nNarrowBlendCamera = regs.edi;
                        fNarrowBlend = 0.0f;
                    }

                    // So the near pass follows the camera on exactly the frames the world is
                    // clamped. The blend out counts, or the viewmodel jumps out of every scene.
                    if (nContext == NARROW_CUTSCENE)
                        nNarrowBlendContext = NARROW_CUTSCENE;

                    bNearPassFollowsCamera.store(
                        nContext == NARROW_CUTSCENE
                            || (fNarrowBlend > 0.0f && nNarrowBlendContext == NARROW_CUTSCENE),
                        std::memory_order_relaxed);

                    if (!bNarrow && fNarrowBlend <= 0.0f)
                    {
                        ClearNarrowViewmodel();
                        return;
                    }

                    // Both contexts take the same ceiling; NarrowContext still tells them apart.
                    if (bNarrow)
                        fNarrowBlendCap = fNarrowFieldOfView;

                    auto fDelta = std::clamp(*(float*)(regs.ebp + nCameraDeltaTime), 0.0f, fNarrowBlendMaxStep);
                    auto fStep = fDelta / fNarrowBlendSeconds;

                    fNarrowBlend = std::clamp(fNarrowBlend + (bNarrow ? fStep : -fStep), 0.0f, 1.0f);
                    if (fNarrowBlend <= 0.0f)
                    {
                        nNarrowBlendContext = NARROW_NONE;
                        ClearNarrowViewmodel();
                        bNearPassFollowsCamera.store(false, std::memory_order_relaxed);
                        return;
                    }

                    // Smoothstep, so the move leaves and arrives at rest.
                    auto fWeight = fNarrowBlend * fNarrowBlend * (3.0f - 2.0f * fNarrowBlend);
                    auto fFov = DegreesToRadians(fNarrowBlendCap);

                    auto pWorldFov = (float*)(regs.esi + nCameraStateFieldOfView);
                    auto fBefore = *pWorldFov;

                    BlendFieldOfView(pWorldFov, fFov, fWeight);
                    BlendFieldOfView((float*)(regs.esi + nCameraStateViewmodelFieldOfView), fFov, fWeight);

                    // Off the world FOV, not the blend, so an unmoved frame carries a scale of one.
                    auto fBeforeTan = std::tan(fBefore * 0.5f);
                    fNarrowTangentScale.store(
                        fBeforeTan > 0.0f ? std::tan(*pWorldFov * 0.5f) / fBeforeTan : 1.0f,
                        std::memory_order_relaxed);

                    fNarrowViewmodelCeiling.store(fFov, std::memory_order_relaxed);
                });
            }

            // Hang gliders: the camera FOV is the vehicle's fFOVAngle, pushed into the pawn's
            // BaseFOV channel as a blend target. Degrees here; inlined twice, hence two patterns.
            auto gliderFovPattern = dunia_pattern("8B CB E8 ? ? ? ? D9 86 30 02 00 00 D9 58 2C 8B 8E 54 02 00 00 89 48 3C F3 0F 10 86 34 02 00 00 F3 0F 59 05 ? ? ? ? F3 0F 11 40 38 C6 40 29 01");
            if (!gliderFovPattern.empty())
            {
                static auto GliderFovHook = safetyhook::create_mid(gliderFovPattern.get_first(0x19), OnSeatFieldOfView);
            }

            auto gliderSeatFovPattern = dunia_pattern("8B 4C 24 08 74 ? E8 ? ? ? ? D9 86 30 02 00 00 D9 58 2C 8B 8E 54 02 00 00 89 48 3C F3 0F 10 86 34 02 00 00 F3 0F 59 05 ? ? ? ? F3 0F 11 40 38 C6 40 29 01");
            if (!gliderSeatFovPattern.empty())
            {
                static auto GliderSeatFovHook = safetyhook::create_mid(gliderSeatFovPattern.get_first(0x1D), OnSeatFieldOfView);
            }

            // Ironsight FOV, taken where the weapon hands it to the pawn: property +0xE4 copied
            // into the channel at +0x1C, both radians. Never the weapon's field, which broke it.
            auto ironsightFovPattern = dunia_pattern("D9 80 E4 00 00 00 D9 5F 1C");
            if (!ironsightFovPattern.empty())
            {
                static auto IronsightFovHook = safetyhook::create_mid(ironsightFovPattern.get_first(nIronsightFovCopied), [](SafetyHookContext& regs)
                {
                    // Every pawn arms itself through here, so the player's channel is picked out
                    // first. An unset handle is accepted, the session's first setup being his.
                    const auto nPlayer = nPawnFieldOfView.load(std::memory_order_relaxed);
                    if (nPlayer != 0 && regs.edi != nPlayer)
                        return;

                    // EDI is the pawn's FOV channels. Taken whether or not the setting is in use:
                    // it is the handle a later change needs.
                    RememberPawnFieldOfView(regs.edi);

                    // The player's melee weapon, for the swing. EAX is the property object.
                    RememberMeleeWeapon(*(uint32_t*)(regs.eax + nWeaponClass));

                    // Degrees for the comparison, both ends of this path being radians. Decided
                    // either way: the push needs the answer for whatever is in hand.
                    const auto fWeapon = *(const float*)(regs.eax + nWeaponIronsightFieldOfView);
                    const auto bOptic = RadiansToDegrees(fWeapon) < fMagnifiedOpticCutoff;

                    bIronsightIsMagnifiedOptic.store(bOptic, std::memory_order_relaxed);

                    if (fIronsightFieldOfView <= 0.0f || bOptic)
                        return;

                    *(float*)(regs.edi + nPawnFovIronsightValue) = DegreesToRadians(fIronsightFieldOfView);
                });
            }
        };
    }
} FieldOfView;
