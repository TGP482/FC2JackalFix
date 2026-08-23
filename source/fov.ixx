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

// Camera controller stores base FOV at +0x70. Replacing the setter value changes world FOV only,
// not weapon aiming FOV.
static constexpr float fPi = 3.14159265f;

static float fFieldOfView = 75.0f;
static float fViewmodelFieldOfView = 75.0f;
static float fIronsightFieldOfView = 0.0f;
static float fVehicleFieldOfView = 0.0f;

// Cutscenes, ladders and gliders break above the stock 75: the cinematic camera reframes shots,
// and the ladder and glider poses show the edges of the first person body. 45 is the floor
// FieldOfView itself clamps to.
static constexpr float fFieldOfViewFloor = 45.0f;

// Hang gliders. Measured before the engine's Hor+ stretch, so this renders as 91.31 on 16:9.
static constexpr float fGliderFieldOfViewMax = 75.0f;

// Cutscenes and ladders. 59.85 comes back out at 75 after the stretch (identity at 4:3, where
// 0.75 * aspect is 1), which is what a 4:3 player sees in stock.
//
// Both share one ceiling: a ladder mount takes control away, so the pawn sits in a scene context
// for the mount animation, and separate ceilings made the view pull to 59.85 for the mount and
// open back to 75 for the climb. Split here if that changes.
static constexpr float fNarrowFieldOfViewMax = 59.85f;

static float fGliderFieldOfView = fGliderFieldOfViewMax;
static float fNarrowFieldOfView = fNarrowFieldOfViewMax;

static float DegreesToRadians(float fDegrees)
{
    return fDegrees * (fPi / 180.0f);
}

// Narrows only, so a value already below the cap survives. Spelled out rather than std::min,
// which Windows.h's min macro breaks.
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

// CCameraBoneComponent::Update gets the secondary base, four bytes into the object, so fFOV
// (+0x70, radians) is reached at +0x6C and the Cinematic flag (+0x90) at +0x8C.
static constexpr uintptr_t nBoneCameraFieldOfView = 0x6C;

// The live camera state CCameraPawnComponent::Update fills in. +0x28 world FOV, +0x30 first
// person model FOV, both radians, both untransformed at the hook below.
//
// FUN_10451ae0 fetches this 0x84 block copy on write, so its address changes between frames and
// nothing may be keyed on it.
static constexpr uintptr_t nCameraStateFieldOfView = 0x28;
static constexpr uintptr_t nCameraStateViewmodelFieldOfView = 0x30;

// Active, CCameraComponent +0x10, reached from the secondary base the update is handed.
static constexpr uintptr_t nCameraActive = 0x0C;

// Frame delta: the update's first argument, passed through to the base update four instructions
// past the hook.
static constexpr uintptr_t nCameraDeltaTime = 0x08;

// Roughly the mount animation length, so the clamp arrives over it instead of snapping when the
// beautifier context changes.
static constexpr float fNarrowBlendSeconds = 0.35f;

// A frame delta past this is a load or a hitch; stepping the blend by one would be the snap the
// blend exists to avoid.
static constexpr float fNarrowBlendMaxStep = 0.1f;

// Only touched from the first person camera update, so no atomics.
//
// Keyed on the component, never on the camera state: keyed on the state, the owner test passes
// only on frames the pool happens to reuse the buffer, so the FOV strobes.
static float fNarrowBlend = 0.0f;
static uintptr_t nNarrowBlendCamera = 0;

// Held rather than re-read on the way out, or the context clearing at the end of a scene swaps
// the cutscene ceiling for the wider one mid blend and pops.
static float fNarrowBlendCap = fNarrowFieldOfViewMax;

// CPawnBeautifierComponent. ContextBeautifier: the instance picked for the pawn's current context.
static constexpr uintptr_t nContextBeautifier = 0x28;

// TypeBeautifier: the player/AI half. AI pawns carry CPawnBeautifierAI, the player
// CPawnBeautifierPlayer, which separates the local player from the other pawns updating this frame.
static constexpr uintptr_t nTypeBeautifier = 0x2C;

// What a beautifier instance means here: the context half names the situation, the type half
// names whose pawn it is, so one classifier covers both.
//
// In engine scenes that keep the player in first person leave the ordinary CCameraPawnComponent
// driving the view, so CCameraBoneComponent::Update is never reached and the cutscene hook below
// cannot see them. There the player's context reads CPawnBeautifierDominoPlayer. CinematicFirst
// and FirstNoControl are the other two non-gameplay first person contexts.
enum BeautifierKind : uint32_t
{
    KIND_OTHER = 0,
    KIND_LADDER,
    KIND_CUTSCENE,
    KIND_POSE,
    KIND_PLAYER,
};

// What the first person camera is currently in, if anything.
enum NarrowContext : uint32_t
{
    NARROW_NONE = 0,
    NARROW_LADDER,
    NARROW_CUTSCENE,

    // Vehicle repairs and machete interrogations. Framed like a cutscene, so same ceiling and
    // near pass rule, but the player's arms and tool are still the subject of the shot, so the
    // first person layer stays with them. See InScriptedPose.
    NARROW_POSE,
};

// CVehicle. sName and the paraglider hash are the pair glider.ixx uses; fFOVAngle sits in the FOV
// block registered by FUN_100dea10 (fFOVTransitionTime +0x230, fFOVAngle +0x234,
// archFOVCurveName +0x238).
static constexpr uintptr_t nVehicleName = 0x00;
static constexpr uintptr_t nVehicleFieldOfViewAngle = 0x234;
static constexpr uint32_t nVehicleNameParaglider = 0x7B2D589C;

// Set only from the pawn whose TypeBeautifier is CPawnBeautifierPlayer, so an AI on a ladder or
// in its own scene cannot narrow the player's view. Matching the camera's target pointer against
// the beautifier's pawn does not work: different objects on different entities, differing ids.
//
// Stamped every frame and read with an expiry rather than latched: written from the reselect,
// it only updates when the context changes, and quickloading out of a scene destroys the pawn
// holding the context with no transition to clear it, leaving the clamp on for the session.
static std::atomic<uint32_t> nNarrowContext = NARROW_NONE;
static std::atomic<uint32_t> nNarrowStamp = 0;

// Long enough to ride out a stutter, short enough that a load releases the clamp unseen.
static constexpr uint32_t nNarrowFreshnessMs = 250;

// Whether the near pass follows the camera outright this frame. Published by the pawn camera hook,
// the only place that knows whether the clamp is being applied; see NearPassFieldOfView.
static std::atomic<bool> bNearPassFollowsCamera = false;

// CPawnFOV+0x34, the vehicle channel weight, sampled once a frame off the blend. Zero on foot,
// one in a seat: the engine's own answer to whether a vehicle drives the FOV.
static std::atomic<float> fVehicleFovWeight = 0.0f;

static bool VehicleOwnsFieldOfView()
{
    return fVehicleFovWeight.load(std::memory_order_relaxed) > 0.0f;
}

static bool NarrowStateIsStale()
{
    return GetTickCount() - nNarrowStamp.load(std::memory_order_relaxed) > nNarrowFreshnessMs;
}

// ---------------------------------------------------------------------------------------------
// Naming a beautifier instance.
//
// Dunia puts a GetClassInfo on primary vtable slot 1 of every class, returning a lazily built
// descriptor whose first field is the .rdata name literal. Verified on CCameraComponent
// (vtable 0x10E70A70 slot 1 -> 0x10050690 -> "CCameraComponent"), CCameraBoneComponent and
// CCameraPawnComponent.
//
// Capturing each vtable from a class-exclusive function would need one exclusive entry point per
// context, and only CPawnBeautifierLadder::OnActivate has one; CPawnBeautifierCinematicFirst
// shares base implementations. So the name is asked for, once per vtable, with every pointer on
// the way range and page checked, so a class breaking the convention gives a wrong answer rather
// than a fault.

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

static bool Readable(const void* p, size_t nBytes)
{
    return IsReadableStruct((uintptr_t)p, nBytes);
}

struct DuniaClassInfo
{
    const char* szName;
    uint32_t nDepth;
};

using tGetClassInfo = const DuniaClassInfo* (__thiscall*)(const void*);

static const char* ClassName(uintptr_t nObject)
{
    if (!Readable((const void*)nObject, sizeof(void*)))
        return "";

    auto pVtable = *(void***)nObject;
    if (!InDunia(pVtable) || !Readable(pVtable, 2 * sizeof(void*)) || !InDunia(pVtable[1]))
        return "";

    auto pClassInfo = ((tGetClassInfo)pVtable[1])((const void*)nObject);
    if (!InDunia(pClassInfo) || !Readable(pClassInfo, sizeof(DuniaClassInfo)))
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
    if (nInstance == 0 || !Readable((const void*)nInstance, sizeof(void*)))
        return KIND_OTHER;

    // A component pointing at a freed instance can read garbage here. Refusing to cache those
    // keeps a stale pointer from colliding with a real class.
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
    else if (std::strcmp(szClass, "CPawnBeautifierDominoPlayer") == 0)
        nKind = KIND_CUTSCENE;
    // Measured: repairs and interrogations both put the player's context here one frame after the
    // weapon's first person layer is taken away; FirstNoControl is the outro of the same move.
    // Ordinary gameplay is CPawnBeautifierFirst, which is KIND_OTHER.
    else if (std::strcmp(szClass, "CPawnBeautifierCinematicFirst") == 0
          || std::strcmp(szClass, "CPawnBeautifierFirstNoControl") == 0)
        nKind = KIND_POSE;
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

// The near pass gets the camera's current FOV, which already includes ironsight zoom. Draw the
// weapon at ViewmodelFieldOfView, and follow the camera once the camera is the narrower of the
// two (aiming, a scope, binoculars), which keeps the sights lined up with the world and arrives
// at IronsightFieldOfView by arithmetic, so it holds for scopes and per weapon values too.
//
// A fixed widening ratio faded out over the top 15 percent of tangent space does not work: at a
// wide FieldOfView that window is the first few degrees of the aim sweep. At FieldOfView 140 the
// weapon went 91.31 -> 144.25 degrees within 7 percent of the transition and swept back down. It
// also left the weapon unscaled in vehicles, whose camera sits below the window whenever
// VehicleFieldOfView is under FieldOfView.
// How far the sights are up, sampled once a frame from the blend. Zero with the weapon down, one
// with the sights fully raised.
static std::atomic<float> fIronsightBlend = 0.0f;

// How far the ladder or cutscene clamp has pulled the world in, in tangent space, and the ceiling
// that clamp is heading for. One is zero when the other is.
//
// The near pass FOV is absolute (ViewmodelFieldOfView), so on a ladder the weapon kept its
// gameplay width while the world narrowed around it. Following the camera instead is no better:
// it shows the world's FieldOfView rather than the setting, and cannot tell a ladder from a pair
// of sights. Scaling by the clamp keeps weapon and world at the same tangent ratio on a ladder as
// in gameplay. The ceiling covers a ViewmodelFieldOfView wide enough that even the scaled result
// would sit above the band the clamp holds.
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

    // The camera state is clamped before the widescreen stretch, so the ceiling is measured there
    // too and takes the stretch here rather than carrying it.
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

    // Wider than the world is a setting the player is allowed to ask for: a Viewmodel FOV above
    // Field of View pushes the gun away. A flat ceiling of one forbade it outright.
    //
    // Except with the sights up, where the gun belongs where the sights put it and the world FOV
    // has already been pulled in to the ironsight FOV, so widening the near pass back out holds
    // the gun away from the eye. The ceiling is kept only while the sights are raised.
    return fIronsightBlend.load(std::memory_order_relaxed) > 0.0f ? 1.0f : fScale;
}

// Only rewrite unmagnified sights, magnified optics use the same property.
static constexpr float fMagnifiedOpticCutoff = 40.0f;

// The weapon property object's ironsight FOV, at the one place the game reads it.
//
// Radians, unlike the vehicle's (degrees, converted by the seat). Nothing converts on this path,
// the weapon setup copying the field straight into the pawn's ironsight channel, so the
// magnified-optic cutoff must be compared in degrees against the converted value.
static constexpr uintptr_t nWeaponIronsightFieldOfView = 0xE4;

// Past the FLD and the FSTP that copy it into the channel, six bytes and three, so the hook lands
// after the copy with both registers still the ones it used.
static constexpr size_t nIronsightFovCopied = 9;

static float RadiansToDegrees(float fRadians)
{
    return fRadians * (180.0f / fPi);
}

// The first person camera's own FOV, on the primary base.
//
// Not 6Ch: the bone camera's update gets the secondary base, so its fFOV is at 6Ch (constant
// above), but CCameraPawnComponent::Update gets the primary base, where 6Ch is the first person
// model's FOV. Its else branch stores 104h into the world half of the camera state and 6Ch into
// the model half:
//
//     10694334  D9 87 04 01..   FLD  [EDI+0x104]    ; world
//     1069433a  D9 5E 28        FSTP [ESI+0x28]
//     1069433d  D9 47 6C        FLD  [EDI+0x6C]     ; first person model
//     10694344  D9 5E 30        FSTP [ESI+0x30]
//
// Writing 6Ch there writes the world FOV into the model's, which is what stopped Field of View
// and Viewmodel FOV being independent. 70h is what the setter writes and what the blend below
// starts from.
static constexpr uintptr_t nPawnCameraFieldOfView = 0x70;

// ---------------------------------------------------------------------------------------------
// The pawn's FOV channels.
//
// Ironsight FOV and Vehicle FOV are not read where they are set: both are pushed into a CPawnFOV
// struct (the weapon at setup, the seat when somebody gets in) and the engine blends from there.
// Overriding either push only reaches the next weapon setup or the next seat, which left the
// ironsight setting doing nothing in a session and the vehicle setting waiting for a remount.
//
// The struct is two channel records of 0x1C bytes each, laid out
//
//     +0x00  a fixed pointer, written once by the constructor
//     +0x04  two flags
//     +0x08  transition time
//     +0x0C  running state, stepped by the blend
//     +0x10  running state
//     +0x14  the target value
//     +0x18  the transition curve
//
// with record A at 0x08 (ironsight, target at 0x1C, degrees) and record B at 0x24 (base, target at
// 0x38, radians). Both boundaries are the constructor's: it writes the same pointer to 0x08 and
// 0x24 and the same curve global to 0x20 and 0x3C, exactly 0x1C apart.
//
// So the values can be re-pushed from outside, which is what happens below: the seat's own two
// writes, with a new number, when the player asks for it.
static constexpr uintptr_t nPawnFovIronsightValue = 0x1C;   // radians

// Record A's blend weight: how far this channel has taken over, zero to one. Read never written,
// being the engine's own running state.
static constexpr uintptr_t nPawnFovIronsightWeight = 0x18;
static constexpr uintptr_t nPawnFovBaseValue      = 0x38;   // radians
static constexpr uintptr_t nPawnFovBaseWeight     = 0x34;
static constexpr uintptr_t nPawnFovBaseArmed      = 0x29;
static constexpr size_t    nPawnFovSize           = 0x60;

// Taken from the two places the engine hands the struct over. No path to it from a global, so it
// is remembered rather than looked up, and checked against its vtable before reuse, since the
// owning pawn does not survive a load.
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

    // Learned from the first struct the engine hands over, so no pattern is spent finding the
    // class's vtable to recognise a later one.
    if (nPawnFieldOfViewVTable.load(std::memory_order_relaxed) == 0)
        nPawnFieldOfViewVTable.store(*(uintptr_t*)nStruct, std::memory_order_relaxed);
}

// The struct, or nothing if the owning pawn has gone: a freed one fails the page test or no
// longer carries CPawnFOV's vtable.
static uintptr_t LivePawnFieldOfView()
{
    auto nStruct = nPawnFieldOfView.load(std::memory_order_relaxed);
    auto nVTable = nPawnFieldOfViewVTable.load(std::memory_order_relaxed);

    if (nStruct == 0 || nVTable == 0 || !IsReadableStruct(nStruct, nPawnFovSize))
        return 0;

    return *(uintptr_t*)nStruct == nVTable ? nStruct : 0;
}

// Re-pushes both channels from the settings. The armed flags are set the way the seat sets them,
// and only where already set, so a player on foot is never handed a vehicle's FOV and the
// ironsight channel is only restarted if something is using it. The targets are written either
// way, so the next aim or seat reads the new number even when nothing is running now.
/*
  Whether the weapon in the player's own hands is a magnified optic.

  There are two paths and the cutoff was only applied on the weapon setup one. The other is the
  push below, which writes straight into the pawn's channel with no cutoff, so changing Ironsight
  FOV while holding a scoped rifle wrote the ironsight number over the scope's own.

  The push cannot judge it itself: all it has is the channel it is writing, so an already pushed
  value reads back as a scope. The setup hook can, being handed the weapon's own field and never
  touching it, so the answer is worked out there and remembered for the push.

  "Player's" matters: every pawn in the level runs CWeapon's setup, AI constantly. Answering from
  whichever pawn armed itself last means the flag reads "assault rifle" a second after the player
  raised a scope and the next push writes the ironsight setting over the scope's FOV; and an AI
  drawing a scoped rifle sets the flag, stopping the player's ironsight setting from applying.
*/
static std::atomic<bool> bIronsightIsMagnifiedOptic = false;

// Which seat is in hand, for the push below. ApplySeatFieldOfView runs once per seat taken and is
// the only place the paraglider is identified, so the answer is kept.
static std::atomic<bool> bSeatIsParaglider = false;

static void PushPawnFieldOfView()
{
    auto nStruct = LivePawnFieldOfView();
    if (nStruct == 0)
        return;

    if (fIronsightFieldOfView > 0.0f && !bIronsightIsMagnifiedOptic.load(std::memory_order_relaxed))
    {
        // Radians: this channel is a straight copy of the weapon's own radian field, so writing
        // degrees means an FOV of about 3500 degrees, which stopped the viewmodel matching sights.
        //
        // The armed flag is left as the engine has it, since setting a clear one would start a
        // channel nothing asked for.
        *(float*)(nStruct + nPawnFovIronsightValue) = DegreesToRadians(fIronsightFieldOfView);
    }

    if (*(uint8_t*)(nStruct + nPawnFovBaseArmed) == 0)
        return;

    // The paraglider takes the glider ceiling, never VehicleFieldOfView. ApplySeatFieldOfView
    // clamps the vehicle's fFOVAngle at mount, but this push writes the pawn channel the seat
    // feeds and lands after it, so a live VehicleFieldOfView overwrote the clamped value.
    //
    // Written outright, not narrowed: fGliderFieldOfView is already clamp(FieldOfView, 45, 75),
    // and narrowing only lowers the channel, so once at the ceiling, raising FieldOfView in the
    // menu could not move it back up.
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

// ---------------------------------------------------------------------------------------------
// Muzzle particles.
//
// The first person weapon draws with the near pass projection and ViewmodelFieldOfView; its muzzle
// flash and smoke draw with the world one. In stock the two FOVs match, so setting them apart
// takes the weapon off its effects, by more the wider the gap.
//
// The renderer already has the machinery: a particle takes a near bucket when the emitter's layer
// mask intersects the render context's, tested in CSceneParticleEmitterRenderer::Render at
// 0x103cbcfb and 0x103cbe55. The emitter's copy is refreshed every frame from the instance's
// +0x1A4, which only CParticlesSystemInstance::SetFirstPersonLayerMask writes, from five call
// sites. The weapon's spawners are not among them, so a muzzle emitter keeps the constructor's
// zero for the life of the process. Measured in game: context mask 1, emitter mask 0, on every
// emitter in a session.
//
// The spawners were found by census: hooking Start and counting return addresses named
// FUN_10111e40 and FUN_10111f80, which fire in equal bursts with the trigger. CMuzzleFlashManager's
// own blocks were measured never to run, and FUN_1015b970 is a third block of the same shape whose
// bytes differ enough to miss a shared pattern.
//
// The anim notify spawner FUN_1013a800 has the sequence the weapon spawners are missing the middle
// of:
//
//     1013aac2  E8 ? ? ? ?      CALL 0x1032ecd0        ; SetTransform
//     1013aac7  8B BF 2C 01..   MOV  EDI,[EDI+0x12C]   ; the owner's own layer mask
//     1013aacd  57              PUSH EDI
//     1013aad2  E8 ? ? ? ?      CALL 0x1032f410        ; resolve the system handle
//     1013aad9  E8 ? ? ? ?      CALL 0x1032ead0        ; SetFirstPersonLayerMask
//
// Reading the mask from the owner rather than deciding one here keeps this off everything else:
// CGraphicComponent carries 1 on the first person weapon and 0 on every AI. Nothing identifies an
// emitter's owner at render time, so overriding at the bucket test would need a rule of our own.
static constexpr uintptr_t nGraphicFirstPersonMask = 0x12C;

using tSetFirstPersonLayerMask = void (__thiscall*)(void*, uint32_t);

static tSetFirstPersonLayerMask SetFirstPersonLayerMask = nullptr;

// EAX is the resolved CParticlesSystemInstance, one instruction before the Start that consumes it.
// EBX is the owner's CGraphicComponent, which both spawners fetched for GetBoneMatrix and do not
// touch again, so the mask is one read away.
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

// ---------------------------------------------------------------------------------------------
// The machete blade mid swing.
//
// The engine takes the first person layer off the melee weapon for the length of a swing and gives
// it back after, one off/on pair per swing, while the blade goes on being submitted half a unit
// from the eye. In stock both passes share an FOV; once ViewmodelFieldOfView differs, the blade is
// detached and unscaled against the hand.
//
// Both halves are the same instruction, the vtable +0x98 call at 0x10156D03 inside FUN_10156c20,
// reached from one weapon state's vtable at 0x10E21E08: 0x10154DD0 hides and passes 0,
// FUN_10154440 shows and passes 1. Neither the call site nor the frames above separate a swing
// from a swap; timing does. Measured with a tick on every toggle:
//
//     hide 78E84C40 -> show 78E84C40      swing, 1328 ms, nothing else shown between
//     hide 78DE2670 -> show 78DE2670      weapon swap, 4563 ms
//     hide 78DE2670 -> show 85DABA90      mount, other components shown same tick
//
// So the clear is withheld on a hide and handed over on the first of: another component shown, a
// scripted pose entered, fSwingMaxSeconds of gameplay elapsed. Withholding zeroes the mask
// argument, leaving +0x130's bookkeeping to the engine.
//
// The owner came from a node to owner map built at FUN_10336b10. FUN_1051cc90 is the wrong hook
// for that, and why the owner read as unrecoverable: the blade's component never reaches it.
//
// Dead ends. Promoting nodes at the four layer tests, see the block above. Matching the weapon by
// component pointer or entity id: the setup's holder and the graphic component sit on different
// entities, B541842A80350200 against B5310F2300054100 in one equip. Pairing the setup with the
// show: the machete's setup has no show near it and the next show ends a swing. Withholding every
// hide: car, boat, glider and turret mounts hide the weapon through this same call, and holding
// the layer through them drew the arms and the mounted weapon over the vehicle.
static constexpr float fSwingMaxSeconds = 1.6f;

// selWeaponClass, property record registered at 0x100F5149, field offset 0x40. Class 0 is every
// HandToHand archetype. Not sufficient alone, since mounting with the machete in hand runs no
// setup and the class still reads melee, but it keeps out every hide made with a gun in hand.
static constexpr uintptr_t nWeaponClass = 0x40;
static constexpr uint32_t nWeaponClassMelee = 0;

static std::atomic<bool> bMeleeInHand = false;

static void RememberMeleeWeapon(uint32_t nClass)
{
    bMeleeInHand.store(nClass == nWeaponClassMelee, std::memory_order_relaxed);
}

static std::atomic<uintptr_t> nLayerWithheldFrom = 0;
static std::atomic<uint32_t> nLayerWithheldBits = 0;

// Gameplay seconds, stepped by the camera update's own delta. On GetTickCount a pause aged a hold
// that had not run a frame, so the swing was called stale and released under the player.
static std::atomic<float> fLayerWithheldFor = 0.0f;

// The mask reaches node+0x90 only through FUN_1051a6e0, which 0x1051B230 calls at its tail:
//
//     1051a6e0  56 8B F1        PUSH ESI / MOV ESI,ECX      ; the component
//     1051a727  8B 4E 30        MOV  ECX,[ESI+0x30]         ; its object entries
//     1051a738  8B 96 2C 01..   MOV  EDX,[ESI+0x12c]        ; the mask, to each entry's node
//
// Writing +0x12C alone left the component reading 0 while its nodes still carried 1, which is first
// person geometry drawn over the world for as long as the seat lasted.
using tPushFirstPersonLayer = void (__fastcall*)(uintptr_t);

static tPushFirstPersonLayer PushFirstPersonLayer = nullptr;

// Dropped rather than written when the component no longer reads or no longer carries the withheld
// bits, since it has been freed or reused by then.
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

// Called from the near pass hook, which runs in every state. The camera blend is the pawn's own and
// stops once a seat is taken, so a hide made entering a car or a glider was never reconsidered
// there and the arms rode the whole way in the near pass.
// A seat, a ladder or a cutscene. Each takes the weapon out of the player's hands through the same
// call a swing uses, so none may be withheld. A ladder held the layer for fSwingMaxSeconds and drew
// the arms over the rungs.
//
// The narrow half is written on the gameplay tick, so the release side reads it across threads and
// can miss a frame of it. Missing one costs nothing here, since the deadline still ends the hold.
static bool InScriptedPose()
{
    if (fVehicleFovWeight.load(std::memory_order_relaxed) > 0.0f)
        return true;

    if (NarrowStateIsStale())
        return false;

    auto nContext = nNarrowContext.load(std::memory_order_relaxed);

    // NARROW_POSE is deliberately not here. A repair or an interrogation hides the weapon through
    // this same call, but the arms and the tool remain the thing the player is meant to be looking
    // at: released, they draw with the world and the engine bay or the man's shoulder draws over
    // them. Held, they keep the near pass exactly as they do in gameplay.
    return nContext == NARROW_LADDER || nContext == NARROW_CUTSCENE;
}

// A pose lasts as long as the animation, several times fSwingMaxSeconds, so the swing deadline is
// not allowed to age one out from under the player. Bounded all the same: leaving the pose by any
// route, a death or a load included, clears the context and lets the deadline run again.
static bool InHeldPose()
{
    return !NarrowStateIsStale()
        && nNarrowContext.load(std::memory_order_relaxed) == NARROW_POSE;
}

// The deadline, on the gameplay thread where the delta comes from. A swing is back in about 1.3
// seconds, so fSwingMaxSeconds only ever fires for a hide whose show never came: a death, a load,
// or a state nothing else here recognises.
static void StepWithheldLayer(float fDelta)
{
    if (nLayerWithheldFrom.load(std::memory_order_acquire) == 0)
        return;

    if (InHeldPose())
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

// CGraphicComponent's enable/disable form, 0x1051B230, __thiscall(mask, bEnable). At the entry the
// return address is at [ESP], the mask at [ESP+4] and the flag at [ESP+8].
static void HoldFirstPersonLayer(SafetyHookContext& regs)
{
    auto nComponent = regs.ecx;
    if (nComponent == 0)
        return;

    auto pMask = (uint32_t*)(regs.esp + 4);
    auto bEnable = *(uint8_t*)(regs.esp + 8) != 0;

    if (bEnable)
    {
        // The same component coming back is the swing ending, and it already holds the value this
        // call would write.
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

    // Only a component already carrying the layer is eligible, which is the nine the player's first
    // person set is given at load. An AI's machete never has it.
    auto nWithheld = *(uint32_t*)(nComponent + nGraphicFirstPersonMask) & *pMask;
    if (nWithheld == 0)
        return;

    // A second hide with no show between them: the first was a swap as well.
    if (nLayerWithheldFrom.load(std::memory_order_relaxed) != 0)
        ReleaseWithheldLayer();

    // Elapsed and bits before the component. The release runs on the render thread and keys on the
    // component being set, so publishing that first leaves a window where it reads the previous
    // withhold's elapsed time, calls the swing stale on its first frame and releases it.
    fLayerWithheldFor.store(0.0f, std::memory_order_relaxed);
    nLayerWithheldBits.store(nWithheld, std::memory_order_relaxed);
    nLayerWithheldFrom.store(nComponent, std::memory_order_release);

    *pMask = 0;
}

// Map markers (archPlayerMarker, archDiamondMarker, etc.) are 3D entities owned by
// CCompassObjectives. In a vehicle the map moves to the world pass while the markers stay in the
// near pass, which viewmodel scaling then misaligns.
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

// The FOV the near pass ends up drawing with. Both the projection hook and the frustum constants
// have to agree on it, so it is worked out once.
static float NearPassFieldOfView(float fCameraFov, float fAspect)
{
    if (MapIsInVehicle())
        return fCameraFov;

    // The near pass copies its constant block from the world's, FUN_103679f0, and FUN_10367270
    // then rewrites the matrix alone. Everything FUN_10379070 derived from the world camera stays
    // behind, so a near pass at a different field of view is drawn against constants describing a
    // different frustum, and what it draws slides as the camera turns. Rewriting the frustum
    // extents at +0x390..+0x39C alone was measured to make it worse, so the mismatch has more than
    // one carrier.
    //
    // A cutscene needs none of that: the clamp has already pulled the world into the 45 to 75 band
    // the viewmodel wants, so following it leaves no mismatch to slide.
    //
    // Read, never re-derived. Context and freshness are written on the gameplay tick while this
    // runs on the render thread, and NarrowStateIsStale() answered true here through ten seconds
    // of a scene the clamp was holding at 75.01, scaling the near pass to 45.01 against it.
    if (bNearPassFollowsCamera.load(std::memory_order_relaxed))
        return fCameraFov;

    auto fNear = ScaleFov(fCameraFov, ViewmodelScaleFor(fCameraFov, fAspect));

    // In a seat the near pass follows the camera. The body and arms are first person geometry and
    // the cab they sit in is world geometry, so two fields of view put the hands where the wheel
    // is not, by a gap that grows with the offset from screen centre. MapIsInVehicle cannot cover
    // this: markers are only placed while the map is open, so it reads false through a drive
    // nobody opens the map in.
    //
    // Faded over the seat's own fFOVTransitionTime rather than switched, or mounting pops. Tangent
    // space, like the widescreen transform.
    auto fSeat = std::clamp(fVehicleFovWeight.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (fSeat <= 0.0f)
        return fNear;

    auto fNearTan = std::tan(fNear * 0.5f);
    auto fCameraTan = std::tan(fCameraFov * 0.5f);

    return 2.0f * std::atan(fNearTan + (fCameraTan - fNearTan) * fSeat);
}

// Near pass depth reconstruction.
//
// FUN_10367f70 builds the near pass constant block by copying the world's, FUN_103679f0, and then
// rewriting the projection set through FUN_10367270. Slot payloads sit 0x10 past the slot header,
// so the matrices land at +0x30 / +0x80 / +0x1C0 / +0x210 / +0x260.
//
//     10368a01  8B 54 24 20     MOV    EDX,[ESP+0x20]           ; near block
//     10368a05  0F 57 C0        XORPS  XMM0,XMM0
//     10368a08  F3 0F 11 82 ..  MOVSS  [EDX+0x37c],XMM0
//
// By that point +0x210, transpose(inverse(P_near * J)), has been written from the near projection,
// but +0x260 is still the copy the world block left behind. +0x260 is the same matrix with the
// source's column 2, the third stored vector after the transpose in FUN_10367400, scaled by
// 1/(far - near); the shaders reconstruct a view space position from depth through it. Rasterising
// through P_near while reconstructing through inverse(P_world) puts every reconstructed position
// off by the field of view ratio in x and y, in proportion to the offset from screen centre, which
// is exactly a viewmodel that slides as the camera turns.
//
// Rebuilding it from +0x210 by the same rule is a no-op while the two fields of view agree, so a
// stock frame is left as it was. The frustum extents at +0x390..+0x39C carry the same mismatch but
// are consumed as a matched pair with this matrix, which is why rewriting them alone was measured
// to make the slide worse.
//
// The dirty mask does not have to be touched: FUN_1040ef20, at the head of FUN_103679f0, sets
// block+0x08 and block+0x0C to 0xFFFFFFFF before FUN_10367270 runs.
static constexpr uintptr_t nBlockInverseProjection = 0x210;
static constexpr uintptr_t nBlockDepthToView       = 0x260;
static constexpr uintptr_t nBlockNearPlane         = 0x370;
static constexpr uintptr_t nBlockFarPlane          = 0x374;

// Rebuilding +0x260 from +0x210 is close to a no-op while the two fields of view agree, not exactly
// one, so on frames where the projection stood the derived matrix is a pure loss. Published by the
// projection hook on every path, including the ones that write nothing.
static std::atomic<bool> bNearPassRewritten = false;

static void SyncNearPassDepthMatrix(SafetyHookContext& regs)
{
    if (!bNearPassRewritten.load(std::memory_order_relaxed))
        return;

    auto nBlock = *(uintptr_t*)(regs.esp + 0x20);
    if (!IsReadableStruct(nBlock, nBlockFarPlane + sizeof(float)))
        return;

    auto fNear = *(float*)(nBlock + nBlockNearPlane);
    auto fFar  = *(float*)(nBlock + nBlockFarPlane);
    if (fFar - fNear <= 0.0f)
        return;

    auto pInverse = (float*)(nBlock + nBlockInverseProjection);
    auto pDepth   = (float*)(nBlock + nBlockDepthToView);
    auto fScale   = 1.0f / (fFar - fNear);

    for (auto i = 0; i < 16; ++i)
        pDepth[i] = pInverse[i];

    for (auto i = 8; i < 12; ++i)
        pDepth[i] *= fScale;
}

// Shared by the two inlined copies of the seat FOV push.
//
// The paraglider is clamped down to the glider ceiling; every other vehicle takes the setting
// outright. Doing it here rather than during deserialization is what makes Vehicle FOV live: the
// archive value is copied into each vehicle as it spawns, so an override at load only ever reaches
// vehicles that have not been built yet, which is why the setting used to need a reload. The seat
// reads the field afresh every time somebody gets in.
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

// Both seat hooks: EAX is the pawn's FOV channels, the store into its curve slot three
// instructions back, the same struct the weapon setup hands over, and ESI is the vehicle.
static void OnSeatFieldOfView(SafetyHookContext& regs)
{
    RememberPawnFieldOfView(regs.eax);
    ApplySeatFieldOfView(regs.esi);
}

// ---------------------------------------------------------------------------------------------
// Near pass routing is the engine's own: (renderContext+0x24 & node+0x90), tested at FUN_103c6260,
// FUN_103c7750, FUN_103c5950 and FUN_103c45f0. node+0x90 is CGraphicComponent+0x12C, the schema
// property RenderInNearZViewPortID, pushed across by FUN_1051a6e0. The machete blade's component
// carries nought there, so the blade draws with the world projection at the world field of view
// while the arms follow ViewmodelFieldOfView.
//
// Do not promote nodes at those four sites. Overriding the test for any node whose owner flags word
// (node+0x14, a copy of CGraphicComponent+0x130) had 0x500 set promoted around 65 nodes a frame,
// including props 275 units from the eye: the same 0x591 sits on the arms and on world geometry,
// and the only bit that differs between a routed node and an unrouted one is 0x20, which 0x1051B230
// writes to record that the mask is set. That cost turrets and AI drawn without the world's depth
// so they showed through walls, characters sliding under camera rotation whenever the near field of
// view differed from the world's, animation and facial LOD picked as first person, and a
// VirtualQuery per submission.
//
// The node cannot name its owner either. Every pointer shaped field in CSceneGraphicObjectInstance
// is a handle into the render pool at 0x10F96D88, and node+0x70/+0x74 share nothing with the nodes
// the engine routes. The material record's FIRST_PERSON bit (record+0x4C bit 12, written
// 0x1037EF3B) would say it, but the record resolves after all four tests (0x103C6AD7, 0x103C7D4C,
// 0x103C4B83).
//
// Granting the layer to the held weapon's own entity through the setter at 0x1051AAE0 was tried and
// does not reach the blade: the component that draws it mid swing never passes FUN_1051cc90, and
// its node was measured at sites 0 and 2 with no owner recoverable from either side.

class FieldOfView
{
public:
    FieldOfView()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Image bounds for the beautifier name walk. Everything it dereferences past the
            // instance itself is a vtable, a class descriptor or a string literal, all inside
            // Dunia, so this is what makes a stray pointer fail closed.
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

                // Straight into the pawn, rather than waiting for the next weapon setup or the next
                // time a seat is taken to carry it there.
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

            // Near pass for the weapon and arms, a separate projection from the world. ESI is the
            // camera, aspect ratio at +0x18.
            auto viewmodelPattern = dunia_pattern("D9 86 28 02 00 00 D9 1C 24 E8 ? ? ? ? D9 45 14");
            if (!viewmodelPattern.empty())
            {
                static auto ViewmodelFovHook = safetyhook::create_mid(viewmodelPattern.get_first(9), [](SafetyHookContext& regs)
                {
                    ReleaseWithheldLayerIfStale();

                    // With the map open in a vehicle NearPassFieldOfView hands the camera FOV
                    // straight back, so the markers keep their alignment.
                    auto pFov = (float*)regs.esp;
                    *pFov = NearPassFieldOfView(*pFov, *(float*)(regs.esi + 0x18));
                });
            }

            // Tail of the near pass block build, just before the engine zeroes +0x37C.
            auto nearDepthPattern = dunia_pattern("8B 54 24 20 0F 57 C0 F3 0F 11 82 7C 03 00 00");
            if (!nearDepthPattern.empty())
            {
                static auto NearDepthHook = safetyhook::create_mid(nearDepthPattern.get_first(), SyncNearPassDepthMatrix);
            }

            // Taken off the anim notify spawner's own call rather than pattern scanned for, since
            // FUN_1032ead0 has no prologue worth anchoring on.
            auto firstPersonMaskPattern = dunia_pattern("8B BF 2C 01 00 00 57 8D 4C 24 20 E8 ? ? ? ? 8B C8 E8");
            if (!firstPersonMaskPattern.empty())
            {
                auto pCall = firstPersonMaskPattern.get_first<uint8_t>(0x12);
                SetFirstPersonLayerMask = (tSetFirstPersonLayerMask)(pCall + 5 + *(int32_t*)(pCall + 1));
            }

            // The two weapon effect spawners, FUN_10111e40 and FUN_10111f80. Their spawn blocks
            // differ in stack layout, so each takes its own pattern:
            //
            //     10112039  E8 ? ? ? ?      CALL 0x1032f410   ; resolve the system handle
            //     1011203e  8B C8           MOV  ECX,EAX      ; <- hook, EAX system, EBX component
            //     10112040  E8 ? ? ? ?      CALL 0x1032f0a0   ; Start
            //
            // Ahead of Start, matching the order FUN_1013a800 uses: the mask is walked out to every
            // emitter the system owns, so it has to land before any of them are running.
            //
            // The second anchor carries a tail as well as a head. Its head alone matches twice, at
            // 0x1011201c and 0x10cd8694.
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
                PushFirstPersonLayer = (tPushFirstPersonLayer)layerPushPattern.get_first<void>();

            // The melee weapon's layer, held through the swing.
            //
            //     1051b230  80 7C 24 08 00  CMP  byte [ESP+0x8],0x0   ; bEnable
            //     1051b235  56 8B F1        PUSH ESI / MOV ESI,ECX    ; the component
            //     1051b23a  74 1E           JZ   the disable half
            //     1051b23c  8B 8E 30 01..   MOV  ECX,[ESI+0x130]
            auto layerTogglePattern = dunia_pattern("80 7C 24 08 00 56 8B F1 74 1E 8B 8E 30 01 00 00");
            if (!layerTogglePattern.empty())
            {
                static auto LayerToggleHook = safetyhook::create_mid(layerTogglePattern.get_first(), HoldFirstPersonLayer);
            }

            // Marker placement, with ECX the owning CCompassObjectives. Read bInVehicle (+0x28)
            // here rather than caching the component pointer.
            auto compassMarkerPattern = dunia_pattern("55 8B EC 83 E4 F0 81 EC 84 00 00 00 53 56 57 8B 7D 10 8B 77 0C 8B D9");
            if (!compassMarkerPattern.empty())
            {
                static auto CompassMarkerHook = safetyhook::create_mid(compassMarkerPattern.get_first(), [](SafetyHookContext& regs)
                {
                    bMarkersInVehicle.store(*(bool*)(regs.ecx + nCompassInVehicleOffset), std::memory_order_relaxed);
                    nMarkersStamp.store(GetTickCount(), std::memory_order_relaxed);
                });
            }

            // The Widescreen option gates the engine's built-in Hor+ adjustment, but its blend
            // weight can stick at zero after toggling the setting, leaving FOV unwidened until
            // restart. Force the branch and pin the weight to 1.0; it is recomputed every frame,
            // so the write does not accumulate.
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

            // Cutscenes. Camera.Cinematic has no class of its own: it is a CCameraBoneComponent
            // with the Cinematic flag its schema adds at +0x90 set, which is what makes it pull in
            // a CDynLoadComponent and stream the world around the shot. Its Update is the only
            // path from the archetype's fFOV to the render camera, storing it into both the world
            // FOV (+0x28) and the model FOV (+0x30) from two loads of the same field, so rewriting
            // that field ahead of the first FLD covers both. Plain Camera.Bone shares the class
            // and is scripted too, so it is left in; +0x8C is the flag to test on if that ever
            // needs narrowing.
            //
            //     10cd4f55  D9 47 6C        FLD  [EDI+0x6C]     ; fFOV, radians
            //     10cd4f58  8B 54 24 40     MOV  EDX,[ESP+0x40]
            //     10cd4f5c  D9 5E 28        FSTP [ESI+0x28]
            //     10cd4f5f  D9 47 6C        FLD  [EDI+0x6C]
            //     10cd4f63  D9 5E 30        FSTP [ESI+0x30]
            auto cinematicFovPattern = dunia_pattern("D9 47 6C 8B 54 24 40 D9 5E 28 D9 47 6C 52 D9 5E 30");
            if (!cinematicFovPattern.empty())
            {
                static auto CinematicFovHook = safetyhook::create_mid(cinematicFovPattern.get_first(), [](SafetyHookContext& regs)
                {
                    NarrowFieldOfView((float*)(regs.edi + nBoneCameraFieldOfView), DegreesToRadians(fNarrowFieldOfView));
                });
            }

            // Ladders and first person scenes. Neither carries an FOV to intercept: no ladder
            // string in the module is FOV related, CPawnBeautifierLadder holds only
            // angMinLookAngle and angMaxLookAngle, CLadder's schema is steps, users and a climb
            // sound, and CPawnBeautifierCinematicFirst holds look angles and movement speeds. The
            // beautifier context is the state to read instead. Nothing is latched: leaving a
            // ladder at the top or the bottom, jumping off, a scene ending, dying and loading a
            // save all change the context and clear it on the next frame.
            //
            // Hooked at CPawnBeautifierComponent::Update's entry rather than the reselect it
            // calls, whose body is wrapped in "if the context tag differs" and runs only on
            // transitions. ECX is
            // the component's secondary base: [EBP+4] two instructions in is the entity ref at
            // component+0x08 and [EBP+0x10] later on is the Enable bool at component+0x14, so the
            // primary base is ECX-4. ContextBeautifier read here is one frame old, which the blend
            // absorbs.
            //
            // An enter/leave pair on the vtable does not work: slot +0x88 is a shared base
            // implementation three classes point at.
            auto beautifierUpdatePattern = dunia_pattern("55 56 8B E9 57 8B 7D 04 8B 77 0C 83 47 08 01 8B CE E8 ? ? ? ? 83 3D ? ? ? ? 00 75 07");
            if (!beautifierUpdatePattern.empty())
            {
                static auto BeautifierUpdateHook = safetyhook::create_mid(beautifierUpdatePattern.get_first(), [](SafetyHookContext& regs)
                {
                    auto nComponent = regs.ecx - 4;

                    // Every AI in the level updates through here on the same frame, so the type
                    // half is checked first and the rest is skipped for all of them.
                    if (KindOf(*(uintptr_t*)(nComponent + nTypeBeautifier)) != KIND_PLAYER)
                        return;

                    auto nContext = NARROW_NONE;
                    switch (KindOf(*(uintptr_t*)(nComponent + nContextBeautifier)))
                    {
                    case KIND_LADDER:   nContext = NARROW_LADDER; break;
                    case KIND_CUTSCENE: nContext = NARROW_CUTSCENE; break;
                    case KIND_POSE:     nContext = NARROW_POSE; break;
                    default: break;
                    }

                    nNarrowContext.store(nContext, std::memory_order_relaxed);
                    nNarrowStamp.store(GetTickCount(), std::memory_order_relaxed);
                });
            }

            // Field of View, live, at the head of the blend that produces the FOV the frame is
            // drawn with. CCameraPawnComponent::Update calls this once a frame and then stores its
            // result:
            //
            //     10692e4c  MOVSS XMM1,[ESI+0x70]     ; the camera's own FOV, the base of it all
            //     10692e51  MOVSS XMM0,[EDI+0x14]     ; CPawnFOV record B target = +38h, vehicle
            //     10692e5d  MULSS XMM0,[EDI+0x10]     ; ...by record B's weight = +34h
            //               then the same again with record A, = +1Ch ironsight over +18h weight
            //     ->        [ESI+0x108], which the update stores into the camera state
            //
            // So the FOV is rebuilt from ESI+70h every frame, and the setter the mod hooks only
            // ever seeds it. Holding it here is what makes the setting live, and it is the base
            // the two channels lerp away from, so a vehicle or a pair of sights still wins over it
            // exactly as the engine intends.
            //
            // Two things follow from the same listing. Both channel targets are read on every call,
            // so writing them from outside, which is what PushPawnFieldOfView does, lands on the
            // next frame rather than waiting for the next push. And +14h/+18h and +30h/+34h are the
            // blend weights, which is why nothing must be written to them.
            //
            // Anchored on the load of the camera's own FOV rather than the function's entry,
            // because by then both calls to the channel getter have returned: ESI is the camera
            // and EAX is the CPawnFOV struct, freshly fetched and certainly alive. One hook, three
            // jobs: hold the FOV, take a valid handle on the channels, and read how far the sights
            // are up.
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

                    // The swing's own deadline, checked here because this is the one hook in the
                    // module that runs every frame on the gameplay thread.
                    ReleaseWithheldLayerIfStale();
                });
            }

            // CCameraPawnComponent::Update for first person, once both FOV stores have landed and
            // before the base update applies the widescreen Hor+ transform, so this caps the same
            // quantity FieldOfView feeds rather than the widened result. Anchored past the FSTP
            // because one instruction earlier the x87 stack still holds the value it pops.
            //
            //     10694340  8B 4C 24 3C     MOV  ECX,[ESP+0x3C] ; the pawn the camera follows
            //     10694344  D9 5E 30        FSTP [ESI+0x30]
            //     10694347  8A 9E 80 ...    MOV  BL,[ESI+0x80]
            //
            // The update runs once per CCameraPawnComponent, so the blend is tied to the camera
            // that raised it. Any other camera reaching this on the same frame would otherwise
            // step the weight back down and fight the ramp.
            auto pawnCameraFovPattern = dunia_pattern("D9 47 6C 8B 4C 24 3C D9 5E 30 8A 9E 80 00 00 00");
            if (!pawnCameraFovPattern.empty())
            {
                static auto NarrowFovHook = safetyhook::create_mid(pawnCameraFovPattern.get_first(10), [](SafetyHookContext& regs)
                {
                    // Only the camera actually driving the view. An inactive one still updates.
                    if (*(uint8_t*)(regs.edi + nCameraActive) == 0)
                        return;

                    // The swing deadline, on the update's own delta, so a paused game does not age
                    // a hold that has not run a frame.
                    auto fFrame = *(float*)(regs.ebp + nCameraDeltaTime);

                    StepWithheldLayer(std::clamp(fFrame, 0.0f, fNarrowBlendMaxStep));

                    // The pawn that owned the state is gone, so drop the clamp outright. Blending
                    // out of it would not run during a loading screen anyway.
                    if (NarrowStateIsStale())
                    {
                        fNarrowBlend = 0.0f;
                        nNarrowBlendCamera = 0;
                        ClearNarrowViewmodel();
                        bNearPassFollowsCamera.store(false, std::memory_order_relaxed);
                        return;
                    }

                    auto nContext = nNarrowContext.load(std::memory_order_relaxed);
                    auto bNarrow = nContext != NARROW_NONE;

                    // A different active camera means the last one was torn down or handed over.
                    // Start again rather than carry a weight across views, and never hold a
                    // pointer that is not coming back.
                    if (nNarrowBlendCamera != regs.edi)
                    {
                        nNarrowBlendCamera = regs.edi;
                        fNarrowBlend = 0.0f;
                    }

                    // Published here so the near pass follows the camera on exactly the frames the
                    // world is clamped and no others.
                    bNearPassFollowsCamera.store(nContext == NARROW_CUTSCENE || nContext == NARROW_POSE, std::memory_order_relaxed);

                    if (!bNarrow && fNarrowBlend <= 0.0f)
                    {
                        ClearNarrowViewmodel();
                        return;
                    }

                    // Both contexts take the same ceiling. NarrowContext still tells them apart
                    // because that is the line to change if they ever need to differ.
                    if (bNarrow)
                        fNarrowBlendCap = fNarrowFieldOfView;

                    auto fDelta = std::clamp(*(float*)(regs.ebp + nCameraDeltaTime), 0.0f, fNarrowBlendMaxStep);
                    auto fStep = fDelta / fNarrowBlendSeconds;

                    fNarrowBlend = std::clamp(fNarrowBlend + (bNarrow ? fStep : -fStep), 0.0f, 1.0f);
                    if (fNarrowBlend <= 0.0f)
                    {
                        ClearNarrowViewmodel();
                        bNearPassFollowsCamera.store(false, std::memory_order_relaxed);
                        return;
                    }

                    // Smoothstep, so the move leaves and arrives at rest instead of starting at
                    // full speed and stopping dead.
                    auto fWeight = fNarrowBlend * fNarrowBlend * (3.0f - 2.0f * fNarrowBlend);
                    auto fFov = DegreesToRadians(fNarrowBlendCap);

                    auto pWorldFov = (float*)(regs.esi + nCameraStateFieldOfView);
                    auto fBefore = *pWorldFov;

                    BlendFieldOfView(pWorldFov, fFov, fWeight);
                    BlendFieldOfView((float*)(regs.esi + nCameraStateViewmodelFieldOfView), fFov, fWeight);

                    // Measured off the world FOV rather than off the blend, so a frame the clamp
                    // did not move anything on carries a scale of one by arithmetic.
                    auto fBeforeTan = std::tan(fBefore * 0.5f);
                    fNarrowTangentScale.store(
                        fBeforeTan > 0.0f ? std::tan(*pWorldFov * 0.5f) / fBeforeTan : 1.0f,
                        std::memory_order_relaxed);

                    fNarrowViewmodelCeiling.store(fFov, std::memory_order_relaxed);
                });
            }

            // Hang gliders. The glider camera FOV is the vehicle's own fFOVAngle, which the seat
            // code converts and pushes into the BaseFOV channel of the pawn's CPawnFOV as the
            // target of a fFOVTransitionTime blend. Writing the source field leaves that blend and
            // its FOV curve alone. The value is still in degrees at the MOVSS, so the engine's own
            // MULSS behind the hook does the conversion.
            //
            //     100dc119  8B 8E 54 02..   MOV   ECX,[ESI+0x254]  ; resolved FOV curve
            //     100dc11f  89 48 3C        MOV   [EAX+0x3C],ECX
            //     100dc122  F3 0F 10 86 ..  MOVSS XMM0,[ESI+0x234] ; fFOVAngle, degrees
            //     100dc12a  F3 0F 59 05 ..  MULSS XMM0,[0x10E0EED8]
            //     100dc132  F3 0F 11 40 38  MOVSS [EAX+0x38],XMM0  ; CPawnFOV BaseFOV.FovOverride
            //
            // Two sites, the same block inlined twice: OnCharacterEnterSeat, which is the mount,
            // and the seat change path reached from the get-in and swap-seat handlers. The block
            // on its own matches both, so each pattern carries the caller's own instructions ahead
            // of it to stay unique.
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

            // Ironsight FOV, where the weapon hands it to the pawn rather than where the archive
            // hands it to the weapon.
            //
            // fIronsightFOV is declared at offset E4h of the weapon's property object, and there is
            // exactly one place in the whole module that reads it, the weapon setup that copies
            // it, the transition time at ECh and the look sensitivity factor at D0h, into the
            // pawn's FOV channel:
            //
            //     10131613  8B 46 04        MOV  EAX,[ESI+0x04]   ; the weapon's property object
            //     10131616  D9 80 E4 ...    FLD  [EAX+0xE4]       ; fIronsightFOV, radians
            //     1013161c  D9 5F 1C        FSTP [EDI+0x1C]       ; CPawnFOV, ironsight channel
            //
            // Overriding it during deserialization instead only ever reached weapons that had not
            // been built yet, which is what made the setting need a reload. Here it lands the next
            // time a weapon is set up, and PushPawnFieldOfView above covers the rest.
            //
            // Nothing converts on this path: the field is radians and so is the channel it lands
            // in. That matters twice over. The cutoff has to be converted before it is compared,
            // or a radian value never clears forty and the override silently never happens, and
            // the write has to be converted, or the sights are handed a number in the thousands of
            // degrees and the viewmodel stops matching them.
            /*
              The write goes to the channel rather than to the weapon.

              This used to overwrite the weapon's own fIronsightFOV, and that is what broke scopes.
              The cutoff is the only thing keeping a magnified optic out of this, a scope's field
              being a genuinely small number that widening undoes, and the cutoff was being
              compared against the very field the override had already written. So an ironsight FOV
              below forty came back on the next setup looking exactly like a scope, the weapon was
              skipped from then on, and raising the setting afterwards could not rescue it either
              because the test still read the old low number. The row goes down to twenty, so this
              was reachable by anyone who tried it.

              Nothing is written to the weapon now. The hook sits three bytes further on, after

                  10131616  FLD  [EAX+0xE4]      ; the weapon's own value, radians
                  1013161c  FSTP [EDI+0x1C]      ; the pawn's ironsight channel

              has already run, where EAX is still the property object and EDI is still the channel,
              and it replaces what landed in the channel. The weapon's field is read and never
              touched, so the cutoff is always compared against the game's own number: a scope stays
              a scope for the life of the process, and the ironsight setting can be anything the row
              allows without ever being mistaken for one.
            */
            auto ironsightFovPattern = dunia_pattern("D9 80 E4 00 00 00 D9 5F 1C");
            if (!ironsightFovPattern.empty())
            {
                static auto IronsightFovHook = safetyhook::create_mid(ironsightFovPattern.get_first(nIronsightFovCopied), [](SafetyHookContext& regs)
                {
                    /*
                      Whose sights these are, before anything is decided from them.

                      Every pawn in the level arms itself through here, a village full of AI
                      running it constantly, and both things taken below are answers about the
                      player specifically: the channel a later change will be written into, and
                      whether what is in hand is a magnified optic. Taken from whichever pawn armed
                      itself last, the optic answer reads "assault rifle" a second after the player
                      raised a scope, and the next push then writes the ironsight setting over the
                      scope's own FOV. That is the scope following the setting.

                      EDI is the channel being filled. The player's is the one the first person
                      camera reads every frame, and the blend hook above is the only thing that
                      writes it, so comparing against it is exact rather than a guess. Nothing has
                      been learned yet on the very first setup of a session and that one is the
                      player's, so an unset handle is accepted rather than refused.
                    */
                    const auto nPlayer = nPawnFieldOfView.load(std::memory_order_relaxed);
                    if (nPlayer != 0 && regs.edi != nPlayer)
                        return;

                    // EDI is the pawn's FOV channels, which nothing else in the module can reach.
                    // Taken here whether or not the setting is in use, because it is the handle a
                    // later change needs.
                    RememberPawnFieldOfView(regs.edi);

                    // The player's melee weapon, for the swing below. ESI is the holder the setup
                    // walks: +0x00 the CGraphicComponent, +0x04 the property object this hook
                    // already has in EAX. Everything past the player test above is the player's,
                    // so nothing else can be learned here.
                    RememberMeleeWeapon(*(uint32_t*)(regs.eax + nWeaponClass));

                    // In degrees for the comparison, since both ends of this path are radians.
                    // Decided whether or not the setting is in use, because the push above needs
                    // the answer for whatever is in hand and this is the only place it can be
                    // known.
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
