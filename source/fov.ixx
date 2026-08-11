/*
  Credit to https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer
  Some fixes were implemented based on the Far Cry 2 Multi Fixer by FoxAhead.
*/

module;

#include <common.hxx>
#include <atomic>
#include <cstring>

export module fov;

import common;
import dunia;
import settings;

// The camera controller stores base FOV at +0x70. Replacing the setter value changes world FOV
// while leaving weapon aiming FOV untouched.
static constexpr float fPi = 3.14159265f;

static float fFieldOfView = 75.0f;
static float fViewmodelFieldOfView = 75.0f;
static float fIronsightFieldOfView = 0.0f;
static float fVehicleFieldOfView = 0.0f;

// Cutscenes, ladders and hang gliders break above the stock 75: the cinematic camera reframes its
// shots, and the ladder and glider poses show the edges of the first person body. An Almost
// Complete Guide to Far Cry 2 Modding drops the paraglider from 90 to 81 for the same seams. 45 is
// the floor FieldOfView itself clamps to.
static constexpr float fFieldOfViewFloor = 45.0f;

// Hang gliders. Everything here is measured before the engine's Hor+ stretch, so this renders as
// 91.31 on a 16:9 display and is vanilla widescreen framing.
static constexpr float fGliderFieldOfViewMax = 75.0f;

// Cutscenes and ladders. 59.85 comes back out at 75 after the stretch, which is what a 4:3 player
// sees in stock, the stretch being identity at 4:3 where 0.75 * aspect is exactly 1.
//
// Both take the same ceiling. Mounting a ladder takes control away from the player, so the pawn
// sits in a scene context for the mount animation; on separate ceilings the view pulled in to
// 59.85 for the mount and opened back out to 75 for the climb. Split them here if that changes.
static constexpr float fNarrowFieldOfViewMax = 59.85f;

static float fGliderFieldOfView = fGliderFieldOfViewMax;
static float fNarrowFieldOfView = fNarrowFieldOfViewMax;

static float DegreesToRadians(float fDegrees)
{
    return fDegrees * (fPi / 180.0f);
}

// Narrows only, so a value the game or another setting already put below the cap survives.
// Spelled out rather than std::min, which Windows.h's min macro breaks.
static void NarrowFieldOfView(float* pFov, float fCap)
{
    if (*pFov > fCap)
        *pFov = fCap;
}

// Partway to the same cap, for a clamp that has to arrive over several frames.
static void BlendFieldOfView(float* pFov, float fCap, float fWeight)
{
    if (*pFov > fCap)
        *pFov += (fCap - *pFov) * fWeight;
}

// CCameraBoneComponent::Update is handed the secondary base pointer, four bytes into the object,
// so the component's own fFOV (+0x70, radians) is reached at +0x6C and its Cinematic flag
// (+0x90) at +0x8C.
static constexpr uintptr_t nBoneCameraFieldOfView = 0x6C;

// The live camera state CCameraPawnComponent::Update fills in. +0x28 is the world FOV and +0x30
// the first person model FOV, both radians, both still untransformed at the hook below.
//
// FUN_10451ae0 fetches this 0x84 block copy on write: with the dirty bit set it takes a different
// block off a free list or allocates one. Its address changes between frames, so nothing may be
// keyed on it.
static constexpr uintptr_t nCameraStateFieldOfView = 0x28;
static constexpr uintptr_t nCameraStateViewmodelFieldOfView = 0x30;

// Active, CCameraComponent +0x10, reached from the secondary base the update is handed.
static constexpr uintptr_t nCameraActive = 0x0C;

// The frame delta is the update's own first argument, which it passes straight through to the
// base update four instructions past the hook.
static constexpr uintptr_t nCameraDeltaTime = 0x08;

// Roughly the mount animation, so the clamp arrives over it instead of snapping on the frame the
// beautifier context changes.
static constexpr float fNarrowBlendSeconds = 0.35f;

// A frame delta past this is a load or a hitch. Stepping the blend by one would be the snap the
// blend exists to avoid.
static constexpr float fNarrowBlendMaxStep = 0.1f;

// Both only ever touched from the first person camera update, so no atomics.
//
// Keyed on the component, never on the camera state. Keyed on the state the owner test passes
// only on the frames the pool happens to reuse the buffer, so the clamp applies on some frames
// and not others and the FOV strobes.
static float fNarrowBlend = 0.0f;
static uintptr_t nNarrowBlendCamera = 0;

// Held rather than re-read on the way out, or the context clearing at the end of a scene would
// swap the cutscene ceiling for the wider one mid blend and pop before the blend ever finished.
static float fNarrowBlendCap = fNarrowFieldOfViewMax;

// CPawnBeautifierComponent. ContextBeautifier is the instance picked for the pawn's current
// context.
static constexpr uintptr_t nContextBeautifier = 0x28;

// TypeBeautifier is the player/AI half of the pair: AI pawns carry CPawnBeautifierAI and the
// player CPawnBeautifierPlayer. That separates the local player from the dozen other pawns
// updating on the same frame.
static constexpr uintptr_t nTypeBeautifier = 0x2C;

// What a beautifier instance means here. The context half names the situation and the type half
// names whose pawn it is, so one classifier covers both.
//
// The in engine scenes that keep the player in first person leave the ordinary
// CCameraPawnComponent driving the view, so CCameraBoneComponent::Update is never reached and the
// cutscene hook below cannot see them. Through those the player's context reads
// CPawnBeautifierDominoPlayer, including with the script's own cinematic UI bracket up.
// CinematicFirst and FirstNoControl are the other two non-gameplay first person contexts.
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

// CVehicle. sName and the paraglider hash are the same pair glider.ixx uses; fFOVAngle sits in
// the FOV block registered by FUN_100dea10 (fFOVTransitionTime +0x230, fFOVAngle +0x234,
// archFOVCurveName +0x238).
static constexpr uintptr_t nVehicleName = 0x00;
static constexpr uintptr_t nVehicleFieldOfViewAngle = 0x234;
static constexpr uint32_t nVehicleNameParaglider = 0x7B2D589C;

// Set only from the pawn whose TypeBeautifier is CPawnBeautifierPlayer, so an AI on a ladder or
// in a scene of its own cannot narrow the player's view. Matching the camera's target pointer
// against the beautifier's pawn does not work: they are different objects on different entities
// and their entity ids differ.
//
// Stamped every frame and read with an expiry, rather than latched. Written from the reselect the
// beautifier update calls, it only updates when the pawn's context changes, and quickloading out
// of a scene destroys the pawn holding the context with no transition to clear it. The clamp then
// stays on for the rest of the session with FieldOfView doing nothing. On an expiry, anything
// that stops the player's beautifier ticking releases it.
static std::atomic<uint32_t> nNarrowContext = NARROW_NONE;
static std::atomic<uint32_t> nNarrowStamp = 0;

// Long enough to ride out a stutter, short enough that a load releases the clamp before the
// player has a frame to look at.
static constexpr uint32_t nNarrowFreshnessMs = 250;

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
// Capturing each vtable from a function only that class reaches needs one exclusive entry point
// per context, and only CPawnBeautifierLadder::OnActivate has one. CPawnBeautifierCinematicFirst
// shares its base implementations. So the name is asked for, once per vtable rather than once per
// frame, with every pointer on the way range and page checked. A class that does not follow the
// convention gives a wrong answer rather than a fault.

static uintptr_t nDuniaBase = 0;
static uintptr_t nDuniaEnd = 0;

static bool InDunia(const void* p)
{
    auto n = reinterpret_cast<uintptr_t>(p);
    return nDuniaBase != 0 && n >= nDuniaBase && n < nDuniaEnd;
}

static bool Readable(const void* p, size_t nBytes)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;

    auto nAvailable = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize
                    - reinterpret_cast<uintptr_t>(p);
    return nAvailable >= nBytes;
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

    // A component whose instance has been freed still points at it, so the vtable read can be
    // garbage. Refusing to cache those keeps a stale pointer from colliding with a real class.
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

// The near pass gets the camera's current FOV, which already includes ironsight zoom. Draw the
// weapon at ViewmodelFieldOfView, and follow the camera once the camera is the narrower of the
// two, which is aiming, a scope or the binoculars. Following it there keeps the sights lined up
// with the world, and it arrives at IronsightFieldOfView by arithmetic, so it holds for a scope
// and for the game's own per weapon values.
//
// A fixed widening ratio faded out over the top 15 percent of tangent space does not work. At a
// wide FieldOfView that window is the first few degrees of the aim sweep: at FieldOfView 140 the
// weapon went from 91.31 to 144.25 degrees within 7 percent of the transition and swept back
// down, reading as FieldOfView overwriting ViewmodelFieldOfView. It also left the weapon unscaled
// in vehicles, whose camera sits below the window whenever VehicleFieldOfView is under
// FieldOfView.
// How far the sights are up, sampled once a frame from the blend. Zero on foot with the weapon
// down, one with the sights fully raised.
static std::atomic<float> fIronsightBlend = 0.0f;

static float ViewmodelScaleFor(float fovRad, float aspect)
{
    auto fCameraTan = std::tan(fovRad * 0.5f);
    if (fCameraTan <= 0.0f)
        return 1.0f;

    auto fViewmodelTan = std::tan(fViewmodelFieldOfView * (fPi / 360.0f)) * fWidescreenStretch * aspect;
    auto fScale = fViewmodelTan / fCameraTan;

    if (fScale <= 1.0f)
        return fScale;

    // Wider than the world, which is a setting the player is allowed to ask for. A Viewmodel FOV
    // above Field of View pushes the gun away, and there is nothing wrong with that. A flat ceiling
    // of one forbade it outright.
    //
    // Except with the sights up. There the gun belongs where the sights put it, and the world FOV
    // has already been pulled in to the ironsight FOV, so widening the near pass back out is
    // exactly the thing that would hold the gun away from the eye. That case is what the ceiling
    // was really for; it is kept, and only for as long as the sights are actually raised.
    return fIronsightBlend.load(std::memory_order_relaxed) > 0.0f ? 1.0f : fScale;
}

// Only rewrite unmagnified sights, magnified optics use the same property.
static constexpr float fMagnifiedOpticCutoff = 40.0f;

// The weapon property object the ironsight FOV lives in, at the one place the game reads it.
//
// Radians, unlike the vehicle's, which is stored in degrees and converted by the seat on the way
// out. Nothing converts on this path, the weapon setup copying the field straight into the pawn's
// ironsight channel, so both ends of it are radians and the magnified-optic cutoff has to be
// compared in degrees against the converted value rather than against the raw one.
static constexpr uintptr_t nWeaponIronsightFieldOfView = 0xE4;

// Past the FLD and the FSTP that copy it into the channel, six bytes and three, so the hook lands
// where the copy has happened and both registers are still the ones it used.
static constexpr size_t nIronsightFovCopied = 9;

static float RadiansToDegrees(float fRadians)
{
    return fRadians * (180.0f / fPi);
}

// The first person camera's own FOV, on the primary base.
//
// Not 6Ch. The bone camera's update is handed the secondary base, so its fFOV is reached at 6Ch and
// the constant above says so. But CCameraPawnComponent::Update is handed the primary base, and
// there 6Ch is a different field entirely: the first person model's FOV. Its else branch is the
// proof, storing 104h into the world half of the camera state and 6Ch into the model half:
//
//     10694334  D9 87 04 01..   FLD  [EDI+0x104]    ; world
//     1069433a  D9 5E 28        FSTP [ESI+0x28]
//     1069433d  D9 47 6C        FLD  [EDI+0x6C]     ; first person model
//     10694344  D9 5E 30        FSTP [ESI+0x30]
//
// Writing 6Ch there is writing the world FOV into the model's, which is what stopped Field of View
// and Viewmodel FOV being independent of each other. 70h is the one the setter writes and the one
// the blend below starts from.
static constexpr uintptr_t nPawnCameraFieldOfView = 0x70;

// ---------------------------------------------------------------------------------------------
// The pawn's FOV channels.
//
// Ironsight FOV and Vehicle FOV are not read where they are set. Both are pushed into a CPawnFOV
// struct, the weapon pushing its ironsight FOV when the weapon is set up and the seat pushing the
// vehicle's when somebody gets in, and the engine blends from there. Overriding either push only
// reaches the next weapon setup or the next time a seat is taken, which is what left
// the ironsight setting doing nothing at all in a session and the vehicle setting waiting for the
// player to get out and back in.
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
// 0x38, radians). Both boundaries are the constructor's own: it writes the same pointer to 0x08 and
// 0x24 and the same curve global to 0x20 and 0x3C, exactly 0x1C apart.
//
// So the values can be re-pushed from outside, which is what happens below: the same two writes
// the seat makes, with a new number and at the moment the player asks for it.
static constexpr uintptr_t nPawnFovIronsightValue = 0x1C;   // radians
static constexpr uintptr_t nPawnFovIronsightArmed = 0x0D;

// Record A's blend weight, the third field the blend reads: how far this channel has taken over,
// nought to one. Read and never written, since it is the engine's own running state.
static constexpr uintptr_t nPawnFovIronsightWeight = 0x18;
static constexpr uintptr_t nPawnFovBaseValue      = 0x38;   // radians
static constexpr uintptr_t nPawnFovBaseArmed      = 0x29;
static constexpr size_t    nPawnFovSize           = 0x60;

// Taken from the two places the engine hands the struct over. There is no path to it from a global,
// so it is remembered rather than looked up, and checked against its own vtable before it is used
// again, because the pawn that owns it does not survive a load.
static std::atomic<uintptr_t> nPawnFieldOfView = 0;
static std::atomic<uintptr_t> nPawnFieldOfViewVTable = 0;

static bool IsReadableStruct(uintptr_t nAddress, size_t nSize)
{
    if (nAddress == 0)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((const void*)nAddress, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return false;

    if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;

    auto nEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return nAddress + nSize <= nEnd;
}

static void RememberPawnFieldOfView(uintptr_t nStruct)
{
    // Called once a frame from the blend, so the same pointer arriving again costs a compare rather
    // than a page query.
    if (nStruct == 0 || nStruct == nPawnFieldOfView.load(std::memory_order_relaxed))
        return;

    if (!IsReadableStruct(nStruct, nPawnFovSize))
        return;

    nPawnFieldOfView.store(nStruct, std::memory_order_relaxed);

    // Learned from the first struct the engine hands over, which is one by construction, so no
    // pattern has to be spent finding the class's vtable to recognise a later one.
    if (nPawnFieldOfViewVTable.load(std::memory_order_relaxed) == 0)
        nPawnFieldOfViewVTable.store(*(uintptr_t*)nStruct, std::memory_order_relaxed);
}

// The struct as it stands, or nothing if the pawn that owned it has gone. A freed one either fails
// the page test or no longer carries CPawnFOV's vtable.
static uintptr_t LivePawnFieldOfView()
{
    auto nStruct = nPawnFieldOfView.load(std::memory_order_relaxed);
    auto nVTable = nPawnFieldOfViewVTable.load(std::memory_order_relaxed);

    if (nStruct == 0 || nVTable == 0 || !IsReadableStruct(nStruct, nPawnFovSize))
        return 0;

    return *(uintptr_t*)nStruct == nVTable ? nStruct : 0;
}

// Re-pushes both channels with what the settings now say. The armed flags are what the seat sets
// after it writes its value, so they are set the same way here, and only where they are already
// set, so a player on foot is never handed a vehicle's FOV and the ironsight channel is only
// restarted if something is already using it. The targets themselves are written either way, so the
// next aim or the next seat reads the new number even when nothing is running now.
/*
  Whether the weapon in the player's own hands is a magnified optic.

  The cutoff was only ever applied on the weapon setup path, and there are two paths. The other one
  is here: a change to the setting pushes straight into the pawn's channel so it takes hold without
  waiting for the next weapon, and that push had no cutoff on it at all, so changing Ironsight FOV
  while holding a scoped rifle wrote the ironsight number over the scope's own, which is exactly the
  scope moving with the ironsight setting.

  The push cannot make the judgement itself: all it has is the channel, and the channel is the
  thing being written, so a value already pushed would read back as a scope. The setup hook can,
  because it is handed the weapon's own field and never touches it. So the answer is worked out
  there, where it is a fact, and remembered for the push to use.

  The word "player's" is the rest of it. CWeapon's setup is not the player's alone: every pawn in
  the level runs it, and a village full of AI runs it constantly. Answering the question from
  whichever pawn happened to arm itself last means the flag reads "assault rifle" a second after
  the player raised a scope, and the next push then writes the ironsight setting over the scope's
  own FOV. That is the scope following the setting. It reads the other way round too: an AI drawing
  a scoped rifle sets the flag and the player's ironsight setting stops applying until something
  else moves it.
*/
static std::atomic<bool> bIronsightIsMagnifiedOptic = false;

static void PushPawnFieldOfView()
{
    auto nStruct = LivePawnFieldOfView();
    if (nStruct == 0)
        return;

    if (fIronsightFieldOfView > 0.0f && !bIronsightIsMagnifiedOptic.load(std::memory_order_relaxed))
    {
        // Radians. This channel is a straight copy of the weapon's own field, which is radians, so
        // writing degrees here is writing an FOV of about three and a half thousand degrees, which
        // is what stopped the viewmodel matching the sights.
        *(float*)(nStruct + nPawnFovIronsightValue) = DegreesToRadians(fIronsightFieldOfView);

        if (*(uint8_t*)(nStruct + nPawnFovIronsightArmed) != 0)
            *(uint8_t*)(nStruct + nPawnFovIronsightArmed) = 1;
    }

    if (fVehicleFieldOfView > 0.0f && *(uint8_t*)(nStruct + nPawnFovBaseArmed) != 0)
    {
        *(float*)(nStruct + nPawnFovBaseValue) = DegreesToRadians(fVehicleFieldOfView);
        *(uint8_t*)(nStruct + nPawnFovBaseArmed) = 1;
    }
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

// Shared by the two inlined copies of the seat FOV push.
//
// The paraglider is clamped down to the glider ceiling; every other vehicle takes the setting
// outright. Doing it here rather than during deserialization is what makes Vehicle FOV live: the
// archive value is copied into each vehicle as it spawns, so an override at load only ever reaches
// vehicles that have not been built yet, which is why the setting used to need a reload. The seat
// reads the field afresh every time somebody gets in.
static void ApplySeatFieldOfView(uintptr_t nVehicle)
{
    if (*(uint32_t*)(nVehicle + nVehicleName) == nVehicleNameParaglider)
    {
        NarrowFieldOfView((float*)(nVehicle + nVehicleFieldOfViewAngle), fGliderFieldOfView);
        return;
    }

    if (fVehicleFieldOfView > 0.0f)
        *(float*)(nVehicle + nVehicleFieldOfViewAngle) = fVehicleFieldOfView;
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

            // Near pass uses a separate projection for weapons and arms, allowing viewmodel FOV
            // changes without affecting the world.
            pattern = dunia_pattern("D9 86 28 02 00 00 D9 1C 24 E8 ? ? ? ? D9 45 14");
            if (pattern.empty())
                return;

            static auto ViewmodelFovHook = safetyhook::create_mid(pattern.get_first(9), [](SafetyHookContext& regs)
            {
                auto pFov = (float*)regs.esp;
                *pFov = ScaleFov(*pFov, fViewmodelScale);
            });*/

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
                    // In vehicles the near-pass FOV stays at the world FOV, so map markers keep
                    // their alignment.
                    if (MapIsInVehicle())
                        return;

                    auto pFov = (float*)regs.esp;
                    *pFov = ScaleFov(*pFov, ViewmodelScaleFor(*pFov, *(float*)(regs.esi + 0x18)));
                });
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

                    // The pawn that owned the state is gone, so drop the clamp outright. Blending
                    // out of it would not run during a loading screen anyway.
                    if (NarrowStateIsStale())
                    {
                        fNarrowBlend = 0.0f;
                        nNarrowBlendCamera = 0;
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

                    if (!bNarrow && fNarrowBlend <= 0.0f)
                        return;

                    // Both contexts take the same ceiling. NarrowContext still tells them apart
                    // because that is the line to change if they ever need to differ.
                    if (bNarrow)
                        fNarrowBlendCap = fNarrowFieldOfView;

                    auto fDelta = std::clamp(*(float*)(regs.ebp + nCameraDeltaTime), 0.0f, fNarrowBlendMaxStep);
                    auto fStep = fDelta / fNarrowBlendSeconds;

                    fNarrowBlend = std::clamp(fNarrowBlend + (bNarrow ? fStep : -fStep), 0.0f, 1.0f);
                    if (fNarrowBlend <= 0.0f)
                        return;

                    // Smoothstep, so the move leaves and arrives at rest instead of starting at
                    // full speed and stopping dead.
                    auto fWeight = fNarrowBlend * fNarrowBlend * (3.0f - 2.0f * fNarrowBlend);
                    auto fFov = DegreesToRadians(fNarrowBlendCap);

                    BlendFieldOfView((float*)(regs.esi + nCameraStateFieldOfView), fFov, fWeight);
                    BlendFieldOfView((float*)(regs.esi + nCameraStateViewmodelFieldOfView), fFov, fWeight);
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
                static auto GliderFovHook = safetyhook::create_mid(gliderFovPattern.get_first(0x19), [](SafetyHookContext& regs)
                {
                    // EAX is the pawn's FOV channels; the store into its curve slot is three
                    // instructions back. Same struct the weapon setup hands over.
                    RememberPawnFieldOfView(regs.eax);
                    ApplySeatFieldOfView(regs.esi);
                });
            }

            auto gliderSeatFovPattern = dunia_pattern("8B 4C 24 08 74 ? E8 ? ? ? ? D9 86 30 02 00 00 D9 58 2C 8B 8E 54 02 00 00 89 48 3C F3 0F 10 86 34 02 00 00 F3 0F 59 05 ? ? ? ? F3 0F 11 40 38 C6 40 29 01");
            if (!gliderSeatFovPattern.empty())
            {
                static auto GliderSeatFovHook = safetyhook::create_mid(gliderSeatFovPattern.get_first(0x1D), [](SafetyHookContext& regs)
                {
                    // EAX is the pawn's FOV channels; the store into its curve slot is three
                    // instructions back. Same struct the weapon setup hands over.
                    RememberPawnFieldOfView(regs.eax);
                    ApplySeatFieldOfView(regs.esi);
                });
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
