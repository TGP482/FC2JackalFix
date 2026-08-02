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
static float ViewmodelScaleFor(float fovRad, float aspect)
{
    auto fCameraTan = std::tan(fovRad * 0.5f);
    if (fCameraTan <= 0.0f)
        return 1.0f;

    auto fViewmodelTan = std::tan(fViewmodelFieldOfView * (fPi / 360.0f)) * fWidescreenStretch * aspect;
    auto fScale = fViewmodelTan / fCameraTan;

    return fScale > 1.0f ? 1.0f : fScale;
}

// Dunia identifies properties by CRC-32 hash, so one can be intercepted without knowing its
// owning class or offset.
static constexpr uint32_t PropertyHash(std::string_view name)
{
    uint32_t nCrc = 0xFFFFFFFF;
    for (auto c : name)
    {
        nCrc ^= static_cast<uint8_t>(c);
        for (auto nBit = 0; nBit < 8; ++nBit)
            nCrc = (nCrc >> 1) ^ (0xEDB88320 & (0u - (nCrc & 1u)));
    }
    return ~nCrc;
}

// Sanity check that we're using Dunia's CRC variant.
static_assert(PropertyHash("fFOVAngle") == 0x49745480);

// Vehicle camera FOV (degrees).
static constexpr uint32_t nVehicleFovProperty = PropertyHash("fFOVAngle");

// Weapon ironsight FOV (radians), unlike the vehicle FOV above.
static constexpr uint32_t nIronsightFovProperty = PropertyHash("fIronsightFOV");

// Only rewrite unmagnified sights, magnified optics use the same property.
static constexpr float fMagnifiedOpticCutoff = 40.0f;

// The descriptor is overwritten before the second hook runs, so cache the hash. Thread-local
// because property streams are parsed on multiple threads.
static thread_local uint32_t nPendingProperty = 0;

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
static void ClampGliderFieldOfView(uintptr_t nVehicle)
{
    if (*(uint32_t*)(nVehicle + nVehicleName) != nVehicleNameParaglider)
        return;

    NarrowFieldOfView((float*)(nVehicle + nVehicleFieldOfViewAngle), fGliderFieldOfView);
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

            // Near pass uses a separate projection for weapons and arms, allowing viewmodel FOV changes
            // without affecting the world.
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
                    // In vehicles, keep the near-pass FOV at the world FOV so map markers stay aligned.
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
            // Hooked at CPawnBeautifierComponent::Update's entry, not the reselect it calls, whose
            // body is wrapped in "if the context tag differs" and runs only on transitions. ECX is
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
                    ClampGliderFieldOfView(regs.esi);
                });
            }

            auto gliderSeatFovPattern = dunia_pattern("8B 4C 24 08 74 ? E8 ? ? ? ? D9 86 30 02 00 00 D9 58 2C 8B 8E 54 02 00 00 89 48 3C F3 0F 10 86 34 02 00 00 F3 0F 59 05 ? ? ? ? F3 0F 11 40 38 C6 40 29 01");
            if (!gliderSeatFovPattern.empty())
            {
                static auto GliderSeatFovHook = safetyhook::create_mid(gliderSeatFovPattern.get_first(0x1D), [](SafetyHookContext& regs)
                {
                    ClampGliderFieldOfView(regs.esi);
                });
            }

            // Weapon and vehicle FOV come from FCB archives, so override them during property
            // deserialization. Installed only when enabled, since this is a hot load path.
            if (fIronsightFieldOfView > 0.0f || fVehicleFieldOfView > 0.0f)
            {
                auto dataPropertyPattern = dunia_pattern("8B 54 24 04 8D 44 24 04 50 83 C2 04 52 E8 ? ? ? ? 85 C0 74 0D 8B 00 8B 4C 24 08 89 01 B0 01 C2 08 00");
                if (!dataPropertyPattern.empty())
                {
                    // Entry: the descriptor is still at [ESP+4].
                    static auto DataPropertyNameHook = safetyhook::create_mid(dataPropertyPattern.get_first(), [](SafetyHookContext& regs)
                    {
                        nPendingProperty = *(uint32_t*)(*(uintptr_t*)(regs.esp + 4) + 4);
                    });

                    // Property found. Replace the parsed value in EAX before it is written out.
                    static auto DataPropertyFovHook = safetyhook::create_mid(dataPropertyPattern.get_first(0x18), [](SafetyHookContext& regs)
                    {
                        auto pValue = (float*)&regs.eax;

                        if (nPendingProperty == nIronsightFovProperty)
                        {
                            if (fIronsightFieldOfView > 0.0f && *pValue * (180.0f / fPi) >= fMagnifiedOpticCutoff)
                                *pValue = fIronsightFieldOfView * (fPi / 180.0f);
                        }
                        else if (nPendingProperty == nVehicleFovProperty)
                        {
                            if (fVehicleFieldOfView > 0.0f)
                                *pValue = fVehicleFieldOfView;
                        }
                    });
                }
            }
        };
    }
} FieldOfView;
