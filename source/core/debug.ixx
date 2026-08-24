module;

#include <common.hxx>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <intrin.h>
#include <string_view>
#include <chrono>

export module debug;

import common;
import dunia;
import settings;
import inputdevice;

static constexpr ptrdiff_t nProfileGodMode = 0x94;
static constexpr ptrdiff_t nProfileUnlimitedAmmo = 0x98;
static constexpr ptrdiff_t nProfileAllWeaponsUnlock = 0xA0;

// Consumers compare unsigned, so a negative value reads as false.
static constexpr int32_t nCheatOn = 1;
static constexpr int32_t nCheatOff = 0;

static constexpr ptrdiff_t nEconomyDiamondCount = 0x10;

// Property descriptor: +0x04 name, +0x08 name hash, +0x0C field offset, +0x10 flags.
static constexpr ptrdiff_t nPropertyHash = 0x08;
static constexpr ptrdiff_t nPropertyOffset = 0x0C;

// CRC-32 of the property name.
static constexpr uint32_t nHashDiamondCount = 0x333DBF78;     // "DiamondCount"
static constexpr uint32_t nHashLastDiamondCount = 0x3A8909F7; // "LastDiamondCount"

// Offsets from the start of a pattern, not from the function entry.
static constexpr ptrdiff_t nInputPassLivePawn = 0x16;
static constexpr ptrdiff_t nEconomyCtorVTableSet = 0x1A;

// CPawnInputListener's look accumulators. Y horizontal, X vertical.
static constexpr ptrdiff_t nListenerLookX = 0x10;
static constexpr ptrdiff_t nListenerLookY = 0x14;

// HUD mirrors: same hash, different object, so the offset is part of the test.
static constexpr int32_t nHudDiamondCount = 0x2BC;
static constexpr int32_t nHudLastDiamondCount = 0x2C8;

// Cameras.

static constexpr ptrdiff_t nPlayerScene = 0x04;
static constexpr ptrdiff_t nSceneCameraManager = 0xB8;
static constexpr ptrdiff_t nManagerFocusLow = 0x08;
static constexpr ptrdiff_t nManagerFocusHigh = 0x0C;
static constexpr ptrdiff_t nManagerLocked = 0x14;

// Vtable slot the manager hands a camera its focus through.
static constexpr size_t nCameraSetFocusSlot = 0x78 / sizeof(void*);

// An entity ref holder: refcount, then the entity.
static constexpr ptrdiff_t nEntityRefCount = 0x08;
static constexpr ptrdiff_t nEntityRefEntity = 0x0C;

// Into the pattern spanning the manager's resolve-and-set-focus sequence.
static constexpr ptrdiff_t nRefWorldGlobal = 0x0F;
static constexpr ptrdiff_t nEntityRefFromIdCall = 0x13;

// CCameraFreeComponent; CCameraGhostComponent derives without adding fields. Move axes are local
// frame, look values rates, 180 deg/sec per unit.
static constexpr ptrdiff_t nCameraMoveForward = 0xB4;
static constexpr ptrdiff_t nCameraMoveStrafe = 0xB8;
static constexpr ptrdiff_t nCameraMoveVertical = 0xBC;
static constexpr ptrdiff_t nCameraLookYaw = 0xC0;
static constexpr ptrdiff_t nCameraLookPitch = 0xC4;
static constexpr ptrdiff_t nCameraSpeed = 0xC8;
static constexpr ptrdiff_t nCameraSpeedAdjust = 0xCC;

// m/s at the baseline step, written to the camera's speed field, not folded into the axes.
static constexpr float fFreecamBaseSpeed = 12.0f;

static constexpr const char* pCameraGameplay = "Cameras.Camera.First";
static constexpr const char* pCameraFree = "Cameras.Camera.Free";

// 4x4 row-major: rows 0-2 basis, row 3 translation.
static constexpr ptrdiff_t nEntityMatrix = 0x30;

// Enable slot the ghost camera's activate handler zeroes.
static constexpr size_t nPhysicsEnabledSlot = 0xB4 / sizeof(void*);

// Into the ghost camera's tag-registration and component-fetch pattern.
static constexpr ptrdiff_t nPhysicsTagFlag = 0x02;
static constexpr ptrdiff_t nPhysicsTagRegisterCall = 0x0B;
static constexpr ptrdiff_t nPhysicsTagValue = 0x11;
static constexpr ptrdiff_t nGetComponentCall = 0x17;

// Block off pawn+0x10: requested vs current state, each with a flags byte carrying the sprint bit.
static constexpr ptrdiff_t nPawnStateBlock = 0x10;
static constexpr ptrdiff_t nPawnRequestedState = 0x140;
static constexpr ptrdiff_t nPawnCurrentState = 0x2D0;

static constexpr ptrdiff_t nStateFlags = 0x04;
static constexpr uint8_t nSprintFlag = 0x40;

// Falling, same block: flag is what other systems query, counter is frames the fall has run.
static constexpr ptrdiff_t nPawnFalling = 0x49B;
static constexpr ptrdiff_t nPawnFallFrames = 0x4A0;

// Into the fall update's pattern: the tail that reports not falling.
static constexpr ptrdiff_t nFallUpdateTail = 0x303;

// CFCXCountersComponentPlayerSP, the campaign player's vitals.

static constexpr ptrdiff_t nCountersHealth = 0x44;   // the CCounter the health lives on
static constexpr ptrdiff_t nCountersForcedFailure = 0x88;
static constexpr ptrdiff_t nCountersRescueState = 0xE8;
static constexpr ptrdiff_t nCountersReviveInvulnerable = 0x140;

// Health-failure threshold, 80.0 from the constructor, raised at runtime: re-read every frame.
static constexpr ptrdiff_t nCountersFailureThreshold = 0x6C;

// Into the constructor's pattern: both vtables written, ESI still the object.
static constexpr ptrdiff_t nCountersCtorVTableSet = 0x15;

// The syringe consume pattern ends on the CALL opcode; the return address is the byte past it.
static constexpr ptrdiff_t nSyringeConsumeReturn = 0x11;
static constexpr size_t nSyringeConsumeSites = 3;

// CCounter. SetToMax drives the HUD/event chain a direct value write would skip.
static constexpr ptrdiff_t nCounterValue = 0x10;
static constexpr size_t nCounterSetToMaxSlot = 0x20 / sizeof(void*);

// CVehiclePhysComponent+0x08: ref block, laid out like an entity ref holder.

static constexpr ptrdiff_t nVehicleRefBlock = 0x08;

// Angle triples here are (pitch, roll, yaw).
static constexpr size_t nAnglePitch = 0;

// The engine integrates look as angle += delta * frameTime * 180 deg, negated for yaw.
static constexpr float fLookRadiansPerUnit = 3.14159265f;

// XINPUT LEFT_THUMB / RIGHT_THUMB, speed step up/down, read only while a mode is up. Left thumb is
// also sprint, free to reuse: noclip clears the sprint request.
static constexpr uint16_t nPadSpeedCycle = 0x0040;
static constexpr uint16_t nPadSlowCycle = 0x0080;

// The signals noclip refuses, by dispatcher CRC32.
static constexpr uint32_t nSignalPauseMenu = 0x04127107; // "show_pausemenu"
static constexpr uint32_t nSignalQuickSave = 0xEFEF8B90; // "quicksave"
static constexpr uint32_t nSignalQuickLoad = 0x9F8F5553; // "quickload"

// The euler triple the renderer reads off the render camera.
static constexpr ptrdiff_t nRenderCameraEuler = 0x6C;

// m/s at the baseline step; the engine's own free camera starts at 5.
static constexpr float fNoclipBaseSpeed = 12.0f;

// Fixed bindings. Only the two mode keys are configurable.
static constexpr int nKeyForward = 'W';
static constexpr int nKeyBack = 'S';
static constexpr int nKeyStrafeLeft = 'A';
static constexpr int nKeyStrafeRight = 'D';
static constexpr int nKeyUp = VK_SPACE;
static constexpr int nKeyDown = VK_LCONTROL;
static constexpr int nKeySpeedCycle = VK_LSHIFT;
static constexpr int nKeySlowCycle = VK_LMENU;
static constexpr int nKeyLookLeft = VK_LEFT;
static constexpr int nKeyLookRight = VK_RIGHT;
static constexpr int nKeyLookUp = VK_UP;
static constexpr int nKeyLookDown = VK_DOWN;

// A whole unit of look rate is a half turn/sec, too fast for a key.
static constexpr float fLookRate = 0.35f;

// Stepped: Shift up the array, Alt down, each wrapping, so one key alone returns to normal speed.
static constexpr float fSpeedSteps[]{ 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f };
static constexpr size_t nBaseSpeedStep = 3;

// Noclip's ceiling; the two steps above are freecam only.
static constexpr size_t nNoclipTopStep = 7;

// Engine functions.

// Null outside a session.
using GetLocalPlayer_t = void* (__cdecl*)();

// __thiscall on the manager. Plain string, not std::string, matched case-insensitively.
using SetActiveCameraByName_t = void(__fastcall*)(void* pManager, void* pEdx, const char* pName, int32_t bNotify);

// __thiscall on the manager. Null when no active camera.
using GetActiveCamera_t = void* (__fastcall*)(void* pManager);

// __thiscall on the ref world, callee cleans 12 bytes; caller owns a reference on the holder.
using EntityRefFromId_t = void* (__fastcall*)(void* pRefWorld, void* pEdx, void** ppOut, uint32_t nIdLow, uint32_t nIdHigh);

// Through the vtable. Holder by value: caller adds a reference, callee drops it.
using SetFocus_t = void(__fastcall*)(void* pCamera, void* pEdx, void* pEntityRef);

// Destroying a holder at zero is these two calls, in this order.
using ReleaseEntityRef_t = void(__fastcall*)(void* pEntityRef);
using FreeEntityRef_t = void(__cdecl*)(void* pEntityRef);

static GetLocalPlayer_t GetLocalPlayer = nullptr;
static SetActiveCameraByName_t SetActiveCameraByName = nullptr;
static GetActiveCamera_t GetActiveCamera = nullptr;
static EntityRefFromId_t EntityRefFromId = nullptr;
static ReleaseEntityRef_t ReleaseEntityRef = nullptr;
static FreeEntityRef_t FreeEntityRef = nullptr;

// The lazy init the engine guards every component fetch with.
using RegisterPhysicsTag_t = void(__fastcall*)(void* pUnused);

// __thiscall on an entity. Returns the component or null.
using GetComponentByTag_t = void* (__fastcall*)(void* pEntity, void* pEdx, void* pTag);

// __thiscall on an entity, three floats by value, callee cleans 12 bytes. SetEuler sits behind
// SetPosition, same shape.
using SetEntityPosition_t = void(__fastcall*)(void* pEntity, void* pEdx, float fX, float fY, float fZ);
using SetEntityEuler_t = void(__fastcall*)(void* pEntity, void* pEdx, float fPitch, float fRoll, float fYaw);

// The pooled render camera a camera component writes into.
using GetRenderCamera_t = void* (__fastcall*)(void* pCamera);

// Vtable slot 0xB4 on the physics component.
using SetPhysicsEnabled_t = void(__fastcall*)(void* pPhysics, void* pEdx, int32_t bEnabled);

// __cdecl, caller cleans. The pawn's current-vehicle fact to its CVehicle component, null on foot.
// Caller owns nothing; waits on the entity's async job, so main thread only.
using GetCurrentVehicle_t = void* (__cdecl*)(void* pPawn);

// __thiscall on an entity. Blocks on its async job, so never called from inside one.
using FlushEntityJob_t = void(__fastcall*)(void* pEntity);

// __thiscall on an entity. Registers its component type on first use, unlike the tag fetch.
using GetVehiclePhysics_t = void* (__fastcall*)(void* pEntity);

// __thiscall on a CCounter, no args. Vtable slot 0x20.
using CounterSetToMax_t = void(__fastcall*)(void* pCounter);

static RegisterPhysicsTag_t RegisterPhysicsTag = nullptr;
static GetComponentByTag_t GetComponentByTag = nullptr;
static GetCurrentVehicle_t GetCurrentVehicle = nullptr;
static FlushEntityJob_t FlushEntityJob = nullptr;
static GetVehiclePhysics_t GetVehiclePhysics = nullptr;
static SetEntityPosition_t SetEntityPosition = nullptr;
static SetEntityEuler_t SetEntityEuler = nullptr;
static GetRenderCamera_t GetRenderCamera = nullptr;

// Read from the instruction that loads it, so it moves with the image.
static void** ppRefWorld = nullptr;

// The physics component's class tag, and whether it has been registered yet.
static void* pPhysicsTag = nullptr;
static int32_t* pPhysicsTagFlag = nullptr;

// Likewise read out of an instruction that dereferences it.
static void** ppGameProfile = nullptr;

// State.

enum class CameraMode
{
    None,
    Noclip,
    Freecam,
};

static CameraMode eCameraMode = CameraMode::None;

// The camera the switch produced, checked against the object being updated.
static void* pDebugCamera = nullptr;

// The manager the switch went through, and its prior Locked byte.
static void* pDebugCameraManager = nullptr;
static uint8_t nSavedLocked = 0;

// Reference held on the player's entity while noclip is engaged.
static void* pNoclipEntityRef = nullptr;
static void* pNoclipPhysics = nullptr;

// The pawn state block noclip holds, so the fall update tells the player's apart. Zero when off.
static uintptr_t nNoclipStateBlock = 0;

// View angles noclip keeps for the player. See ApplyNoclip.
static float fNoclipYaw = 0.0f;
static float fNoclipPitch = 0.0f;

// Set while the camera's axes hold our values, so they come down once on release. Latched apart so
// keyboard movement does not zero the stick.
static bool bFedCameraMove = false;
static bool bFedCameraLook = false;

// Kept across activations, like the camera's own speed field.
static size_t nSpeedStep = nBaseSpeedStep;

// The highest step the mode currently up allows.
static size_t TopSpeedStep()
{
    return (eCameraMode == CameraMode::Noclip) ? nNoclipTopStep : std::size(fSpeedSteps) - 1;
}

// Look input since the camera last consumed it, sampled once a frame from the pawn's accumulators.
static float fMouseLookX = 0.0f;
static float fMouseLookY = 0.0f;

// The player's economy component and the vtable it was first seen with. It dies with the player
// while this module's clock keeps ticking, hence the vtable test before every write.
static void* pEconomy = nullptr;
static void* pEconomyVTable = nullptr;

// Granted diamonds still in the wallet. Real progress is the live count minus this.
static int32_t nGranted = 0;

// Return addresses of the syringe spend sites; the decrementer is shared with all consumables.
static uintptr_t pSyringeConsumeSite[nSyringeConsumeSites]{};
static size_t nSyringeConsumeSiteCount = 0;

// The vitals component and its constructor's vtable, on the same terms as pEconomy.
static void* pCounters = nullptr;
static void* pCountersVTable = nullptr;

// The player's vehicle, re-derived every frame. Read from the damage hooks on physics jobs, so
// written last-to-first and cleared first-to-last: a torn read must fail the component test.
static void* pVehiclePhysics = nullptr;
static void* pVehicleRefBlock = nullptr;
static void* pVehicleEntity = nullptr;

static int nNoclipKey = 0;
static int nFreecamKey = 0;

/*
  The multiplayer gate: every setting here is a cheat, so none may be live in a match.

  Session type, int at +18h on the session object. A match is 2 or 3.
      0 Invalid  1 Offline  2 Online  3 Lan  4 Split  5 Single

  The type is read in the session-change handler and kept, not the slot address: +18h through a dead
  session is allocator garbage. The singleton's flag at +4 is "not Offline", not this test.
*/
static const char* const szSessionSlotPattern =
    "8B 8E E4 00 00 00 E8 ? ? ? ? 83 F8 01 0F 95 C1 88 4C 24 50";
static constexpr uintptr_t nOwnerSession = 0xE4; // disp of mov ecx,[esi+0E4h]
static constexpr uintptr_t nSessionType = 0x18;

static constexpr int32_t nSessionOnline = 2;
static constexpr int32_t nSessionLan = 3;

// Written from the engine thread inside the handler, read from the menu thread and physics jobs.
static std::atomic<int32_t> nLiveSessionType = 0;

export bool JackalFixInMultiplayer()
{
    const auto nType = nLiveSessionType.load();

    return nType == nSessionOnline || nType == nSessionLan;
}

// The ini's answers, gated per read: a match starts and ends without the file moving. Every owning
// path handles its setting going off underneath.
static bool bIniInvincibility = false;
static bool bIniInfiniteAmmo = false;
static bool bIniUnlockAllWeapons = false;
static bool bIniNoclipEnabled = false;
static bool bIniFreecamEnabled = false;
static int32_t nIniDiamondTarget = 0;

static bool Invincibility() { return bIniInvincibility && !JackalFixInMultiplayer(); }
static bool InfiniteAmmo() { return bIniInfiniteAmmo && !JackalFixInMultiplayer(); }
static bool UnlockAllWeapons() { return bIniUnlockAllWeapons && !JackalFixInMultiplayer(); }
static bool NoclipEnabled() { return bIniNoclipEnabled && !JackalFixInMultiplayer(); }
static bool FreecamEnabled() { return bIniFreecamEnabled && !JackalFixInMultiplayer(); }
static int32_t DiamondTarget() { return JackalFixInMultiplayer() ? 0 : nIniDiamondTarget; }

// A name from the table, a decimal virtual key code, or a hex one. Anything else disables the key.
static int ParseKeyName(std::string_view szName)
{
    struct NamedKey
    {
        std::string_view szName;
        int nKey;
    };

    static constexpr NamedKey Keys[]
    {
        { "NONE", 0 }, { "OFF", 0 }, { "0", 0 },
        { "F1", VK_F1 }, { "F2", VK_F2 }, { "F3", VK_F3 }, { "F4", VK_F4 },
        { "F5", VK_F5 }, { "F6", VK_F6 }, { "F7", VK_F7 }, { "F8", VK_F8 },
        { "F9", VK_F9 }, { "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 },
        { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME }, { "END", VK_END },
        { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT },
        { "NUMLOCK", VK_NUMLOCK }, { "SCROLLLOCK", VK_SCROLL }, { "PAUSE", VK_PAUSE },
        { "TAB", VK_TAB }, { "BACKSPACE", VK_BACK }, { "ENTER", VK_RETURN }, { "SPACE", VK_SPACE },
        { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT }, { "UP", VK_UP }, { "DOWN", VK_DOWN },
        { "NUMPAD0", VK_NUMPAD0 }, { "NUMPAD1", VK_NUMPAD1 }, { "NUMPAD2", VK_NUMPAD2 },
        { "NUMPAD3", VK_NUMPAD3 }, { "NUMPAD4", VK_NUMPAD4 }, { "NUMPAD5", VK_NUMPAD5 },
        { "NUMPAD6", VK_NUMPAD6 }, { "NUMPAD7", VK_NUMPAD7 }, { "NUMPAD8", VK_NUMPAD8 },
        { "NUMPAD9", VK_NUMPAD9 },
        { "MULTIPLY", VK_MULTIPLY }, { "ADD", VK_ADD }, { "SUBTRACT", VK_SUBTRACT },
        { "DIVIDE", VK_DIVIDE }, { "DECIMAL", VK_DECIMAL },
    };

    std::string szUpper;
    szUpper.reserve(szName.size());
    for (auto c : szName)
    {
        if (c == ' ' || c == '\t')
            continue;
        szUpper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    if (szUpper.empty())
        return 0;

    for (const auto& key : Keys)
    {
        if (szUpper == key.szName)
            return key.nKey;
    }

    // Letters and digits are their own virtual key.
    if (szUpper.size() == 1 && ((szUpper[0] >= 'A' && szUpper[0] <= 'Z') || (szUpper[0] >= '0' && szUpper[0] <= '9')))
        return szUpper[0];

    auto nBase = (szUpper.rfind("0X", 0) == 0) ? 16 : 10;
    auto nParsed = std::strtol(szUpper.c_str() + (nBase == 16 ? 2 : 0), nullptr, nBase);

    return (nParsed > 0 && nParsed < 256) ? static_cast<int>(nParsed) : 0;
}

// Or the keys fire while the player is alt-tabbed and typing elsewhere.
static bool HasFocus()
{
    DWORD nProcess = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &nProcess);

    return nProcess == GetCurrentProcessId();
}

static bool KeyDown(int nKey)
{
    return nKey != 0 && (GetAsyncKeyState(nKey) & 0x8000) != 0;
}

// Edge triggered on the way down, one latch per key.
static bool KeyPressed(int nKey, bool& bLatch)
{
    auto bDown = KeyDown(nKey);
    auto bEdge = bDown && !bLatch;
    bLatch = bDown;

    return bEdge;
}

static bool PadPressed(uint16_t nButton, bool& bLatch)
{
    auto bDown = (GetPadState().nButtons & nButton) != 0;
    auto bEdge = bDown && !bLatch;
    bLatch = bDown;

    return bEdge;
}

// Unit axes. Left stick adds to the keys where the engine's mapping is not already feeding them;
// height stays on the keyboard, the triggers being aim/fire.
struct MoveAxes
{
    float fStrafe;
    float fForward;
    float fVertical;
};

static MoveAxes ReadMoveAxes(bool bWithPad)
{
    MoveAxes Move
    {
        (KeyDown(nKeyStrafeRight) ? 1.0f : 0.0f) - (KeyDown(nKeyStrafeLeft) ? 1.0f : 0.0f),
        (KeyDown(nKeyForward) ? 1.0f : 0.0f) - (KeyDown(nKeyBack) ? 1.0f : 0.0f),
        (KeyDown(nKeyUp) ? 1.0f : 0.0f) - (KeyDown(nKeyDown) ? 1.0f : 0.0f),
    };

    if (bWithPad)
    {
        const auto& Pad = GetPadState();

        Move.fStrafe = std::clamp(Move.fStrafe + PadThumbAxis(Pad.sThumbLX), -1.0f, 1.0f);
        Move.fForward = std::clamp(Move.fForward + PadThumbAxis(Pad.sThumbLY), -1.0f, 1.0f);
    }

    return Move;
}

// Re-applied every frame: MP spectator clears GodMode, a profile reload writes the file's back.
static void ApplyProfileFlags()
{
    if (ppGameProfile == nullptr)
        return;

    auto pProfile = reinterpret_cast<uintptr_t>(*ppGameProfile);
    if (pProfile == 0)
        return;

    *reinterpret_cast<int32_t*>(pProfile + nProfileGodMode) = Invincibility() ? nCheatOn : nCheatOff;
    *reinterpret_cast<int32_t*>(pProfile + nProfileUnlimitedAmmo) = InfiniteAmmo() ? nCheatOn : nCheatOff;
    *reinterpret_cast<int32_t*>(pProfile + nProfileAllWeaponsUnlock) = UnlockAllWeapons() ? nCheatOn : nCheatOff;
}

// A live local player, and the object still carrying the vtable it was first seen with. Without it
// a level load leaves an int32 written into freed heap.
static bool ObjectIsLive(void* pObject, void* pVTable)
{
    if (pObject == nullptr || pVTable == nullptr)
        return false;

    if (GetLocalPlayer == nullptr || GetLocalPlayer() == nullptr)
        return false;

    return *reinterpret_cast<void**>(pObject) == pVTable;
}

// Puts the player back above the health-failure threshold, which GodMode on its own will not do.
static void ApplyHealthFloor()
{
    if (!Invincibility() || !ObjectIsLive(pCounters, pCountersVTable))
        return;

    auto nCounters = reinterpret_cast<uintptr_t>(pCounters);

    // The states the engine parks the player below the threshold on purpose.
    if (*reinterpret_cast<uint8_t*>(nCounters + nCountersForcedFailure) != 0)
        return;
    if (*reinterpret_cast<int32_t*>(nCounters + nCountersRescueState) != 0)
        return;
    if (*reinterpret_cast<uint8_t*>(nCounters + nCountersReviveInvulnerable) != 0)
        return;

    auto pCounter = *reinterpret_cast<void**>(nCounters + nCountersHealth);
    if (pCounter == nullptr)
        return;

    auto fHealth = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(pCounter) + nCounterValue);
    if (fHealth >= *reinterpret_cast<float*>(nCounters + nCountersFailureThreshold))
        return;

    auto ppVTable = *reinterpret_cast<void***>(pCounter);
    reinterpret_cast<CounterSetToMax_t>(ppVTable[nCounterSetToMaxSlot])(pCounter);
}

// GodMode reaches no vehicle damage path. Drowning and scripted destruction stay lethal.
static bool VehicleIsProtected(void* pComponent)
{
    if (!Invincibility() || pComponent == nullptr || pComponent != pVehiclePhysics)
        return false;

    auto pRefBlock = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pComponent) + nVehicleRefBlock);
    if (pRefBlock == nullptr || pRefBlock != pVehicleRefBlock)
        return false;

    return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pRefBlock) + nEntityRefEntity) == pVehicleEntity;
}

// The pawn keeps no vehicle pointer, so the link is the engine's accessor over the seat fact.
static void ApplyVehicleProtection(uintptr_t nPawn)
{
    pVehiclePhysics = nullptr;
    pVehicleRefBlock = nullptr;
    pVehicleEntity = nullptr;

    if (!Invincibility() || nPawn == 0)
        return;

    if (GetCurrentVehicle == nullptr || GetVehiclePhysics == nullptr)
        return;

    auto pVehicle = GetCurrentVehicle(reinterpret_cast<void*>(nPawn));
    if (pVehicle == nullptr)
        return;

    auto pVehicleBlock = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pVehicle) + nVehicleRefBlock);
    if (pVehicleBlock == nullptr)
        return;

    auto pEntity = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pVehicleBlock) + nEntityRefEntity);
    if (pEntity == nullptr)
        return;

    if (FlushEntityJob != nullptr)
        FlushEntityJob(pEntity);

    auto pPhysics = GetVehiclePhysics(pEntity);
    if (pPhysics == nullptr)
        return;

    // Off the physics component, so the hooks compare against the block they read themselves.
    auto pRefBlock = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pPhysics) + nVehicleRefBlock);
    if (pRefBlock == nullptr)
        return;

    auto pPhysicsEntity = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pRefBlock) + nEntityRefEntity);
    if (pPhysicsEntity == nullptr)
        return;

    pVehicleRefBlock = pRefBlock;
    pVehicleEntity = pPhysicsEntity;
    pVehiclePhysics = pPhysics;
}

static int32_t* LiveDiamonds()
{
    if (pEconomy == nullptr)
        return nullptr;

    return reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pEconomy) + nEconomyDiamondCount);
}

// Hands the grant back; abandoning it would promote granted diamonds to real progress.
static void RetireGrant()
{
    if (nGranted <= 0)
        return;

    if (ObjectIsLive(pEconomy, pEconomyVTable))
    {
        auto pLive = LiveDiamonds();
        *pLive = (*pLive > nGranted) ? (*pLive - nGranted) : 0;
    }

    nGranted = 0;
}

// A different component is a different session. Settle the outgoing wallet before adopting it.
static void NoteEconomy(void* pComponent)
{
    if (pComponent == nullptr || pComponent == pEconomy)
        return;

    RetireGrant();

    pEconomy = pComponent;

    if (pEconomyVTable == nullptr)
        pEconomyVTable = *reinterpret_cast<void**>(pComponent);
}

// The three int32 properties carrying a diamond count into a save: the wallet and two HUD mirrors.
static bool IsDiamondCountProperty(void* pDescriptor)
{
    if (pDescriptor == nullptr)
        return false;

    auto pProperty = reinterpret_cast<uintptr_t>(pDescriptor);
    auto nHash = *reinterpret_cast<uint32_t*>(pProperty + nPropertyHash);
    auto nOffset = *reinterpret_cast<int32_t*>(pProperty + nPropertyOffset);

    if (nHash == nHashDiamondCount)
        return nOffset == nEconomyDiamondCount || nOffset == nHudDiamondCount;

    if (nHash == nHashLastDiamondCount)
        return nOffset == nHudLastDiamondCount;

    return false;
}

// Tops the wallet up every frame; nGranted stays the shortfall between real progress and target.
static void ApplyDiamonds()
{
    // Pointer kept on failure: settling the grant needs a live wallet.
    if (!ObjectIsLive(pEconomy, pEconomyVTable))
        return;

    auto pLive = LiveDiamonds();
    const auto nTarget = DiamondTarget();

    if (nTarget > 0)
    {
        if (*pLive < nTarget)
        {
            nGranted += nTarget - *pLive;
            *pLive = nTarget;
        }
        else if (nGranted > 0 && *pLive > nTarget)
        {
            // Ini lowered: hand back only the part of the grant above the new target.
            auto nOver = *pLive - nTarget;
            auto nBack = (nOver < nGranted) ? nOver : nGranted;

            *pLive -= nBack;
            nGranted -= nBack;
        }

        // Trim the grant to what is still in the wallet, if something outside lowered the count.
        nGranted = std::clamp(nGranted, 0, (*pLive > 0) ? *pLive : 0);

        return;
    }

    // Off with a grant outstanding: hand back what is left.
    RetireGrant();
}

static void* ResolveCameraManager()
{
    if (GetLocalPlayer == nullptr)
        return nullptr;

    auto pPlayer = GetLocalPlayer();
    if (pPlayer == nullptr)
        return nullptr;

    auto pScene = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pPlayer) + nPlayerScene);
    if (pScene == nullptr)
        return nullptr;

    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pScene) + nSceneCameraManager);
}

static void ClearCameraAxes(void* pCamera)
{
    if (pCamera == nullptr)
        return;

    auto pComponent = reinterpret_cast<uintptr_t>(pCamera);

    *reinterpret_cast<float*>(pComponent + nCameraMoveForward) = 0.0f;
    *reinterpret_cast<float*>(pComponent + nCameraMoveStrafe) = 0.0f;
    *reinterpret_cast<float*>(pComponent + nCameraMoveVertical) = 0.0f;
    *reinterpret_cast<float*>(pComponent + nCameraLookYaw) = 0.0f;
    *reinterpret_cast<float*>(pComponent + nCameraLookPitch) = 0.0f;
    *reinterpret_cast<float*>(pComponent + nCameraSpeedAdjust) = 0.0f;
}

// The manager's focus as a ref holder. Caller owns a reference; pass it to ReleaseFocusEntity.
static void* AcquireFocusEntity(void* pManager)
{
    if (pManager == nullptr || EntityRefFromId == nullptr || ppRefWorld == nullptr)
        return nullptr;

    auto pRefWorld = *ppRefWorld;
    if (pRefWorld == nullptr)
        return nullptr;

    auto nIdLow = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(pManager) + nManagerFocusLow);
    auto nIdHigh = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(pManager) + nManagerFocusHigh);

    // The manager's own guard for "no focus set".
    if ((nIdLow & nIdHigh) == 0xFFFFFFFF)
        return nullptr;

    void* pEntityRef = nullptr;
    EntityRefFromId(pRefWorld, nullptr, &pEntityRef, nIdLow, nIdHigh);

    return pEntityRef;
}

static void ReleaseFocusEntity(void*& pEntityRef)
{
    if (pEntityRef == nullptr)
        return;

    auto pRefCount = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pEntityRef) + nEntityRefCount);

    *pRefCount -= 1;
    if (*pRefCount == 0 && ReleaseEntityRef != nullptr && FreeEntityRef != nullptr)
    {
        ReleaseEntityRef(pEntityRef);
        FreeEntityRef(pEntityRef);
    }

    pEntityRef = nullptr;
}

static void* FocusEntity(void* pEntityRef)
{
    if (pEntityRef == nullptr)
        return nullptr;

    return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pEntityRef) + nEntityRefEntity);
}

// Puts the camera back on the player; the manager only does it on the instantiating activation.
static void SnapCameraToFocus(void* pManager, void* pCamera)
{
    if (pCamera == nullptr)
        return;

    auto pEntityRef = AcquireFocusEntity(pManager);
    if (pEntityRef == nullptr)
        return;

    if (FocusEntity(pEntityRef) != nullptr)
    {
        // By value: the callee drops this reference, leaving the one acquired above.
        *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pEntityRef) + nEntityRefCount) += 1;

        auto ppVTable = *reinterpret_cast<void***>(pCamera);
        reinterpret_cast<SetFocus_t>(ppVTable[nCameraSetFocusSlot])(pCamera, nullptr, pEntityRef);
    }

    ReleaseFocusEntity(pEntityRef);
}

static void SetPhysicsEnabled(void* pPhysics, bool bEnabled)
{
    if (pPhysics == nullptr)
        return;

    auto ppVTable = *reinterpret_cast<void***>(pPhysics);
    reinterpret_cast<SetPhysicsEnabled_t>(ppVTable[nPhysicsEnabledSlot])(pPhysics, nullptr, bEnabled ? 1 : 0);
}

// The pitch the view is at, off the pooled render camera. False when there is no camera to ask.
static bool ReadViewPitch(float& fPitch)
{
    if (GetActiveCamera == nullptr || GetRenderCamera == nullptr)
        return false;

    auto pCamera = GetActiveCamera(ResolveCameraManager());
    if (pCamera == nullptr)
        return false;

    auto nRenderCamera = reinterpret_cast<uintptr_t>(GetRenderCamera(pCamera));
    if (nRenderCamera == 0)
        return false;

    fPitch = *reinterpret_cast<float*>(nRenderCamera + nRenderCameraEuler + nAnglePitch * sizeof(float));

    return true;
}

// Owns the mode as well as the state, so no caller has to remember to clear it.
static void LeaveNoclip()
{
    if (eCameraMode == CameraMode::Noclip)
        eCameraMode = CameraMode::None;

    if (pNoclipEntityRef == nullptr)
        return;

    // Collision back before the reference goes, so the last thing touched is still alive.
    SetPhysicsEnabled(pNoclipPhysics, true);

    nNoclipStateBlock = 0;
    pNoclipPhysics = nullptr;
    ReleaseFocusEntity(pNoclipEntityRef);
}

static bool EnterNoclip()
{
    LeaveNoclip();

    if (GetComponentByTag == nullptr || SetEntityPosition == nullptr || pPhysicsTag == nullptr)
        return false;

    auto pEntityRef = AcquireFocusEntity(ResolveCameraManager());
    auto pEntity = FocusEntity(pEntityRef);

    if (pEntity == nullptr)
    {
        ReleaseFocusEntity(pEntityRef);
        return false;
    }

    // The engine's own guard on every fetch of this tag.
    if (pPhysicsTagFlag != nullptr && *pPhysicsTagFlag == 0 && RegisterPhysicsTag != nullptr)
        RegisterPhysicsTag(nullptr);

    auto pPhysics = GetComponentByTag(pEntity, nullptr, pPhysicsTag);
    if (pPhysics == nullptr)
    {
        ReleaseFocusEntity(pEntityRef);
        return false;
    }

    SetPhysicsEnabled(pPhysics, false);

    // Seeded from the body's facing; local +Y is forward, hence the quarter turn.
    auto pMatrix = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(pEntity) + nEntityMatrix);
    fNoclipYaw = std::atan2(pMatrix[5], pMatrix[4]) - 1.57079633f;

    // Pitch only exists on the camera.
    fNoclipPitch = 0.0f;
    ReadViewPitch(fNoclipPitch);

    pNoclipEntityRef = pEntityRef;
    pNoclipPhysics = pPhysics;

    return true;
}

// Runs from the input pass, ahead of the arms animation and the camera that follows the head.
static void ApplyNoclip(uintptr_t nListener, uintptr_t nPawn, float fDelta)
{
    auto pEntity = FocusEntity(pNoclipEntityRef);
    if (pEntity == nullptr)
        return;

    // Sprint goes nowhere but still reaches the animation layer: clear request and current.
    auto nState = (nPawn != 0) ? *reinterpret_cast<uintptr_t*>(nPawn + nPawnStateBlock) : 0;
    nNoclipStateBlock = nState;

    if (nState != 0)
    {
        *reinterpret_cast<uint8_t*>(nState + nPawnRequestedState + nStateFlags) &= static_cast<uint8_t>(~nSprintFlag);
        *reinterpret_cast<uint8_t*>(nState + nPawnCurrentState + nStateFlags) &= static_cast<uint8_t>(~nSprintFlag);
    }

    // Physics off takes the character controller with it, so the body cannot turn and the head bone
    // cannot look; yaw is integrated here off the same accumulator.
    if (nListener != 0 && fDelta > 0.0f)
        fNoclipYaw -= *reinterpret_cast<const float*>(nListener + nListenerLookY) * fDelta * fLookRadiansPerUnit;

    // Pitch only steers flight, so it is read off the camera: integrating it on this module's wall
    // clock drifted against engine frame time. No camera, pitch zero, flight level.
    ReadViewPitch(fNoclipPitch);

    // Body takes the yaw and stays upright: pitching it pitches the head bone and the arms.
    if (SetEntityEuler != nullptr)
        SetEntityEuler(pEntity, nullptr, 0.0f, 0.0f, fNoclipYaw);

    if (fDelta <= 0.0f || !HasFocus())
        return;

    const auto Move = ReadMoveAxes(true);

    if (Move.fStrafe == 0.0f && Move.fForward == 0.0f && Move.fVertical == 0.0f)
        return;

    auto fStep = fNoclipBaseSpeed * fSpeedSteps[nSpeedStep] * fDelta;

    // The entity's basis from the matrix: X strafe, Y forward, Z up. SetEuler already rebuilt it.
    auto pMatrix = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(pEntity) + nEntityMatrix);

    auto pRight = pMatrix;
    auto pAhead = pMatrix + 4;
    auto pUp = pMatrix + 8;

    // The body is upright, so flight pitch is applied here rather than read out of the basis.
    auto fCos = std::cos(fNoclipPitch);
    auto fSin = std::sin(fNoclipPitch);

    float fMove[3]{};
    for (auto i = 0; i < 3; i++)
    {
        auto fAheadTilted = pAhead[i] * fCos + pUp[i] * fSin;
        fMove[i] = (Move.fStrafe * pRight[i] + Move.fForward * fAheadTilted + Move.fVertical * pUp[i]) * fStep;
    }

    SetEntityPosition(pEntity, nullptr,
                      pMatrix[12] + fMove[0],
                      pMatrix[13] + fMove[1],
                      pMatrix[14] + fMove[2]);
}

// A level load destroys the manager and a cutscene may take the camera: ask the engine first.
static bool DebugCameraStillOurs()
{
    if (eCameraMode == CameraMode::None || GetActiveCamera == nullptr)
        return false;

    auto pManager = ResolveCameraManager();
    if (pManager == nullptr || pManager != pDebugCameraManager)
        return false;

    return GetActiveCamera(pManager) == pDebugCamera;
}

// Fields only, so it is also safe from shutdown, which runs under the loader lock.
static void ResetCameraState()
{
    eCameraMode = CameraMode::None;
    pDebugCamera = nullptr;
    pDebugCameraManager = nullptr;
    nSavedLocked = 0;
    bFedCameraMove = false;
    bFedCameraLook = false;
    fMouseLookX = 0.0f;
    fMouseLookY = 0.0f;
}

// Locked byte back if its manager is still live, then forget. Every exit path ends here.
static void ForgetCameraMode()
{
    if (pDebugCameraManager != nullptr && pDebugCameraManager == ResolveCameraManager())
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pDebugCameraManager) + nManagerLocked) = nSavedLocked;

    ResetCameraState();
}

static void LeaveCameraMode()
{
    if (eCameraMode == CameraMode::None)
        return;

    if (DebugCameraStillOurs() && SetActiveCameraByName != nullptr)
    {
        ClearCameraAxes(pDebugCamera);
        SetActiveCameraByName(pDebugCameraManager, nullptr, pCameraGameplay, 1);

        // Gameplay camera did not take: keep the mode, or the key no longer exits it.
        if (GetActiveCamera(pDebugCameraManager) == pDebugCamera)
            return;
    }

    ForgetCameraMode();
}

static bool EnterFreecam()
{
    if (SetActiveCameraByName == nullptr || GetActiveCamera == nullptr)
        return false;

    LeaveCameraMode();

    auto pManager = ResolveCameraManager();
    if (pManager == nullptr)
        return false;

    auto pPrevious = GetActiveCamera(pManager);

    // Locked makes the switch a silent no-op. Clear in ordinary gameplay, but a cutscene may set it.
    auto pLocked = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pManager) + nManagerLocked);
    auto nWasLocked = *pLocked;
    *pLocked = 0;

    SetActiveCameraByName(pManager, nullptr, pCameraFree, 1);

    auto pCamera = GetActiveCamera(pManager);

    // The switch reports nothing, and a prototype the data does not carry leaves the old camera up.
    if (pCamera == nullptr || pCamera == pPrevious)
    {
        *pLocked = nWasLocked;
        return false;
    }

    // Axes and position persist between activations.
    ClearCameraAxes(pCamera);
    SnapCameraToFocus(pManager, pCamera);

    ResetCameraState();

    eCameraMode = CameraMode::Freecam;
    pDebugCamera = pCamera;
    pDebugCameraManager = pManager;
    nSavedLocked = nWasLocked;

    return true;
}

// Camera input onto the camera's own axis fields. The retail free_camera mapping already fills them
// from a pad; only mouse look has no binding, sampled in the tick and consumed here.

static void FeedCameraInput(void* pCamera)
{
    if (pCamera == nullptr)
        return;

    auto pComponent = reinterpret_cast<uintptr_t>(pCamera);
    auto bFocused = HasFocus();

    // Downstream of both the keyboard and the stick.
    *reinterpret_cast<float*>(pComponent + nCameraSpeed) = fFreecamBaseSpeed * fSpeedSteps[nSpeedStep];

    // No pad: the free_camera mapping already feeds these from the stick.
    const auto Move = bFocused ? ReadMoveAxes(false) : MoveAxes{};

    if (Move.fForward != 0.0f || Move.fStrafe != 0.0f || Move.fVertical != 0.0f)
    {
        // Unit axes. The speed is on the field written above.
        *reinterpret_cast<float*>(pComponent + nCameraMoveForward) = Move.fForward;
        *reinterpret_cast<float*>(pComponent + nCameraMoveStrafe) = Move.fStrafe;
        *reinterpret_cast<float*>(pComponent + nCameraMoveVertical) = Move.fVertical;

        bFedCameraMove = true;
    }
    else if (bFedCameraMove)
    {
        // The engine writes these only when an action fires, so a released key coasts. One pass of
        // zeros, then hands off.
        *reinterpret_cast<float*>(pComponent + nCameraMoveForward) = 0.0f;
        *reinterpret_cast<float*>(pComponent + nCameraMoveStrafe) = 0.0f;
        *reinterpret_cast<float*>(pComponent + nCameraMoveVertical) = 0.0f;

        bFedCameraMove = false;
    }

    // The mouse arrives in the keys' units, and is consumed rather than held.
    auto fYawKeys = (bFocused && KeyDown(nKeyLookRight) ? 1.0f : 0.0f) - (bFocused && KeyDown(nKeyLookLeft) ? 1.0f : 0.0f);
    auto fPitchKeys = (bFocused && KeyDown(nKeyLookUp) ? 1.0f : 0.0f) - (bFocused && KeyDown(nKeyLookDown) ? 1.0f : 0.0f);

    auto fYaw = fYawKeys * fLookRate + fMouseLookX;

    // Pitch integrates with a negated scale, so up is negative; the sample already has that sign.
    auto fPitch = -fPitchKeys * fLookRate + fMouseLookY;

    fMouseLookX = 0.0f;
    fMouseLookY = 0.0f;

    if (fYaw != 0.0f || fPitch != 0.0f)
    {
        *reinterpret_cast<float*>(pComponent + nCameraLookYaw) = fYaw;
        *reinterpret_cast<float*>(pComponent + nCameraLookPitch) = fPitch;

        bFedCameraLook = true;
    }
    else if (bFedCameraLook)
    {
        *reinterpret_cast<float*>(pComponent + nCameraLookYaw) = 0.0f;
        *reinterpret_cast<float*>(pComponent + nCameraLookPitch) = 0.0f;

        bFedCameraLook = false;
    }
}

// Hooks.

static SafetyHookInline SaveDiamondPropertyHook{};
static SafetyHookInline GameSignalHook{};
static SafetyHookInline FreeCameraUpdateHook{};
static SafetyHookInline VehicleHealthDamageHook{};
static SafetyHookInline VehicleStimPartsHook{};
static SafetyHookInline ItemConsumeHook{};

// The shared item decrementer: returns early when UnlimitedAmmo is set, which is what made healing
// free. The flag is cleared for the syringe call only and put straight back.
static int32_t __fastcall ItemConsume(void* pPool, void* pEdx, int32_t nAmount)
{
    auto nCaller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto bSyringe = false;

    for (size_t i = 0; i < nSyringeConsumeSiteCount; i++)
    {
        if (nCaller == pSyringeConsumeSite[i])
        {
            bSyringe = true;
            break;
        }
    }

    auto pProfile = (ppGameProfile != nullptr) ? *ppGameProfile : nullptr;

    if (!bSyringe || pProfile == nullptr)
        return ItemConsumeHook.fastcall<int32_t>(pPool, pEdx, nAmount);

    auto pUnlimitedAmmo = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pProfile) + nProfileUnlimitedAmmo);
    auto nSaved = *pUnlimitedAmmo;
    *pUnlimitedAmmo = nCheatOff;

    auto nResult = ItemConsumeHook.fastcall<int32_t>(pPool, pEdx, nAmount);

    *pUnlimitedAmmo = nSaved;

    return nResult;
}

// CVehiclePhysComponent::ApplyHealthDamage, the one place vehicle health is written. Zero passed
// rather than the call skipped, so the health ratio at +0x90 is still recomputed.
static void __fastcall VehicleHealthDamage(void* pComponent, void* pEdx, float fDamage)
{
    if (VehicleIsProtected(pComponent))
        fDamage = 0.0f;

    VehicleHealthDamageHook.fastcall(pComponent, pEdx, fDamage);
}

// CVehiclePhysComponent::ApplyStimToParts, part breakage. Safe to skip: both callers ignore the
// return, and the collision-immunity timer only throttles damage already suppressed.
static void __fastcall VehicleStimParts(void* pComponent, void* pEdx, void* pStim)
{
    if (VehicleIsProtected(pComponent))
        return;

    VehicleStimPartsHook.fastcall(pComponent, pEdx, pStim);
}

// The shared int32 property writer. The value is in a register before anything callable, so the
// field is substituted around the call; serialisation is single threaded.
static void __fastcall SaveDiamondProperty(void* pDescriptor, void* pEdx, void* pObject, void* pVisitor)
{
    if (nGranted > 0 && pObject != nullptr && IsDiamondCountProperty(pDescriptor))
    {
        auto nOffset = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pDescriptor) + nPropertyOffset);
        auto pField = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pObject) + nOffset);

        auto nInflated = *pField;
        *pField = (nInflated > nGranted) ? (nInflated - nGranted) : 0;

        SaveDiamondPropertyHook.fastcall(pDescriptor, pEdx, pObject, pVisitor);

        *pField = nInflated;
        return;
    }

    SaveDiamondPropertyHook.fastcall(pDescriptor, pEdx, pObject, pVisitor);
}

// The dispatcher all three signals are acted on in. True is the original's own refusal: handled,
// nothing done.
static bool __fastcall GameSignal(void* pDispatcher, void* pEdx, const uint32_t* pSignal, void* pContext)
{
    if (eCameraMode == CameraMode::Noclip && pSignal != nullptr)
    {
        auto nSignal = *pSignal;
        if (nSignal == nSignalPauseMenu || nSignal == nSignalQuickSave || nSignal == nSignalQuickLoad)
            return true;
    }

    return GameSignalHook.fastcall<bool>(pDispatcher, pEdx, pSignal, pContext);
}

// CCameraFreeComponent::Update, also called by the ghost camera's, so one hook drives both. ECX is
// the component's second base, four bytes in.
static void __fastcall FreeCameraUpdate(void* pInterface, void* pEdx, float fDelta, uint32_t nFlags)
{
    auto pCamera = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pInterface) - 4);

    if (eCameraMode == CameraMode::Freecam && pCamera == pDebugCamera)
        FeedCameraInput(pCamera);

    FreeCameraUpdateHook.fastcall(pInterface, pEdx, fDelta, nFlags);
}

// AllWeaponsUnlock bypasses the per-weapon unlock list only; these two act gates are the rest,
// patched only while Unlock All Weapons is live.
static raw_mem* pFirstActGate = nullptr;
static raw_mem* pSecondActGate = nullptr;

// -1 rather than a bool so the first call always writes, whichever way it goes.
static int nActGateWritten = -1;

// Every frame as well as on an ini change: a match starts and ends without the file moving. The
// only setting that is a patch rather than a read.
static void ApplyActGate()
{
    if (pFirstActGate == nullptr)
        return;

    const auto nWanted = UnlockAllWeapons() ? 1 : 0;
    if (nWanted == nActGateWritten)
        return;

    nActGateWritten = nWanted;

    if (nWanted != 0)
    {
        pFirstActGate->Write();
        pSecondActGate->Write();
    }
    else
    {
        pFirstActGate->Restore();
        pSecondActGate->Restore();
    }
}

static void ReadSettings()
{
    bIniInvincibility = JackalFixSettings.GetInt(PREF_DEBUGINVINCIBILITY) != 0;
    bIniInfiniteAmmo = JackalFixSettings.GetInt(PREF_DEBUGINFINITEAMMO) != 0;
    bIniUnlockAllWeapons = JackalFixSettings.GetInt(PREF_DEBUGUNLOCKALLWEAPONS) != 0;

    nIniDiamondTarget = JackalFixSettings.GetInt(PREF_DEBUGDIAMONDS);

    bIniNoclipEnabled = JackalFixSettings.GetInt(PREF_DEBUGNOCLIP) != 0;
    bIniFreecamEnabled = JackalFixSettings.GetInt(PREF_DEBUGFREECAM) != 0;

    nNoclipKey = ParseKeyName(JackalFixSettings.GetString(PREF_DEBUGNOCLIPKEY));
    nFreecamKey = ParseKeyName(JackalFixSettings.GetString(PREF_DEBUGFREECAMKEY));

    // One key cannot mean two modes. Noclip is first in the ini, so it keeps the key.
    if (nFreecamKey != 0 && nFreecamKey == nNoclipKey)
        nFreecamKey = 0;

    ApplyActGate();
}

// Once a frame from CPawnInputListener::Update, on the gameplay-input-enabled branch. Not a camera
// update: activating by name there mutates the manager's array mid iteration.
static void Tick(uintptr_t nListener, uintptr_t nPawn)
{
    // The input pass carries no delta. A hitch or a load comes back as zero, not a huge step.
    static std::chrono::steady_clock::time_point LastTick{};

    const auto Now = std::chrono::steady_clock::now();
    auto fFrameDelta = (LastTick == std::chrono::steady_clock::time_point{})
        ? 0.0f : std::chrono::duration<float>(Now - LastTick).count();
    LastTick = Now;

    if (fFrameDelta < 0.0f || fFrameDelta > 0.25f)
        fFrameDelta = 0.0f;

    ApplyActGate();

    ApplyProfileFlags();
    ApplyHealthFloor();
    ApplyVehicleProtection(nPawn);
    ApplyDiamonds();

    // Mouse look for the free camera, off the listener's accumulators. Skipped on a pad, where the
    // engine's mapping already feeds these and would double them.
    if (eCameraMode == CameraMode::Freecam && nListener != 0 && !IsPadActiveDevice())
    {
        fMouseLookX = *reinterpret_cast<float*>(nListener + nListenerLookX);
        fMouseLookY = *reinterpret_cast<float*>(nListener + nListenerLookY);
    }

    if (eCameraMode == CameraMode::Noclip)
    {
        // The entity going away is a level load. Drop the reference rather than follow it.
        if (FocusEntity(pNoclipEntityRef) == nullptr)
            LeaveNoclip();
        else
            ApplyNoclip(nListener, nPawn, fFrameDelta);
    }

    // A level load or a cutscene taking the camera ends the mode where it stands, no switch made.
    if (eCameraMode == CameraMode::Freecam && !DebugCameraStillOurs())
        ForgetCameraMode();

    // A mode switched off mid-use, or a match starting, has to put the player back.
    if (eCameraMode == CameraMode::Noclip && !NoclipEnabled())
        LeaveNoclip();
    else if (eCameraMode == CameraMode::Freecam && !FreecamEnabled())
        LeaveCameraMode();

    if (!HasFocus())
        return;

    static bool bNoclipLatch = false;
    static bool bFreecamLatch = false;
    static bool bSpeedLatch = false;
    static bool bSlowLatch = false;
    auto bNoclipEdge = KeyPressed(nNoclipKey, bNoclipLatch);
    auto bFreecamEdge = KeyPressed(nFreecamKey, bFreecamLatch);

    // Only while a mode is up: outside one, Shift and the left stick click are the sprint binding.
    static bool bPadSpeedLatch = false;
    static bool bPadSlowLatch = false;

    auto bPadSpeedEdge = PadPressed(nPadSpeedCycle, bPadSpeedLatch);
    auto bPadSlowEdge = PadPressed(nPadSlowCycle, bPadSlowLatch);

    if ((KeyPressed(nKeySpeedCycle, bSpeedLatch) || bPadSpeedEdge) && eCameraMode != CameraMode::None)
        nSpeedStep = (nSpeedStep >= TopSpeedStep()) ? nBaseSpeedStep : nSpeedStep + 1;

    if ((KeyPressed(nKeySlowCycle, bSlowLatch) || bPadSlowEdge) && eCameraMode != CameraMode::None)
        nSpeedStep = (nSpeedStep == 0) ? nBaseSpeedStep : nSpeedStep - 1;

    if (NoclipEnabled() && bNoclipEdge)
    {
        if (eCameraMode == CameraMode::Noclip)
        {
            LeaveNoclip();
        }
        else
        {
            LeaveCameraMode();
            if (EnterNoclip())
            {
                eCameraMode = CameraMode::Noclip;

                // A step chosen in freecam can sit past noclip's lower ceiling.
                if (nSpeedStep > nNoclipTopStep)
                    nSpeedStep = nNoclipTopStep;
            }
        }
    }
    else if (FreecamEnabled() && bFreecamEdge)
    {
        if (eCameraMode == CameraMode::Freecam)
        {
            LeaveCameraMode();
        }
        else
        {
            LeaveNoclip();
            EnterFreecam();
        }
    }
}

class Debug
{
public:
    Debug()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // The session-changed handler, at the load of the session it is about to ask about.
            auto sessionSlot = dunia_pattern(szSessionSlotPattern);
            if (!sessionSlot.empty())
            {
                static auto SessionSlotHook = safetyhook::create_mid(sessionSlot.get_first(), [](SafetyHookContext& regs)
                {
                    auto* pSession = *reinterpret_cast<uint8_t**>(regs.esi + nOwnerSession);
                    const auto nType = pSession != nullptr
                        ? *reinterpret_cast<int32_t*>(pSession + nSessionType) : 0;

                    nLiveSessionType = nType;
                });
            }

            ReadSettings();

            // CWeaponBazaar::IsWeaponUnlocked at the AllWeaponsUnlock test, read only for the
            // GameProfile global, the operand of the leading MOV EAX,[imm32].
            auto profilePattern = dunia_pattern("A1 ? ? ? ? 83 B8 A0 00 00 00 00");
            if (!profilePattern.empty())
                ppGameProfile = *reinterpret_cast<void***>(profilePattern.get_first(1));

            // The act gates in the same function: the tag at ESI+0x58 compared against 1 and 2, at
            // pattern +3 and +18. The tag is only ever 0-2, so 0xFF makes both jumps unconditional.
            auto actGatePattern = dunia_pattern("80 7E 58 01 75 09 E8 ? ? ? ? 84 C0 74 11 80 7E 58 02");
            if (!actGatePattern.empty())
            {
                static raw_mem fnFirstActGate(actGatePattern.get_first(3), { 0xFF });
                static raw_mem fnSecondActGate(actGatePattern.get_first(18), { 0xFF });

                pFirstActGate = &fnFirstActGate;
                pSecondActGate = &fnSecondActGate;

                ApplyActGate();

                JackalFix::onShutdownEvent() += []()
                {
                    fnFirstActGate.Restore();
                    fnSecondActGate.Restore();
                };
            }

            // CEconomyComponent::AddDiamonds, only to learn the player's component; count untouched.
            auto addDiamondsPattern = dunia_pattern("51 A1 ? ? ? ? A8 01 56 57 8B F9 75 12 83 C8 01 A3 ? ? ? ? C7 05 ? ? ? ? A8 64 B0 93");
            if (!addDiamondsPattern.empty())
            {
                static auto AddDiamondsHook = safetyhook::create_mid(addDiamondsPattern.get_first(), [](SafetyHookContext& regs)
                {
                    NoteEconomy(reinterpret_cast<void*>(regs.ecx));
                });
            }

            // CEconomyComponent::RemoveDiamonds, before its clamp and store. Spending comes off the
            // grant first, which holds real progress at live - nGranted.
            auto removeDiamondsPattern = dunia_pattern("F6 05 ? ? ? ? 01 53 57 8B D9 75 11");
            if (!removeDiamondsPattern.empty())
            {
                static auto RemoveDiamondsHook = safetyhook::create_mid(removeDiamondsPattern.get_first(), [](SafetyHookContext& regs)
                {
                    NoteEconomy(reinterpret_cast<void*>(regs.ecx));

                    if (nGranted <= 0)
                        return;

                    auto pLive = LiveDiamonds();
                    if (pLive == nullptr)
                        return;

                    // The engine clamps to the balance, so this has to as well.
                    auto nSpent = *reinterpret_cast<int32_t*>(regs.esp + 4);
                    if (nSpent > *pLive)
                        nSpent = *pLive;

                    nGranted -= (nSpent < nGranted) ? nSpent : nGranted;
                });
            }

            // The bazaar page snapshotting the wallet as it opens: one more sighting.
            auto bazaarSnapshotPattern = dunia_pattern("8B 40 10 89 86 70 01 00 00 89 86 74 01 00 00 89 86 78 01 00 00");
            if (!bazaarSnapshotPattern.empty())
            {
                static auto BazaarSnapshotHook = safetyhook::create_mid(bazaarSnapshotPattern.get_first(), [](SafetyHookContext& regs)
                {
                    NoteEconomy(reinterpret_cast<void*>(regs.eax));
                });
            }

            // CConstIntProperty::Serialise, shared by every int32 property, hence filtering the
            // descriptor. ESI takes the value, not the field, so the substitution goes round it.
            auto savePropertyPattern = dunia_pattern("56 8B C1 8B 70 0C 8B 4C 24 0C 8B 11 57 8B 7C 24 0C 8B 34 3E 83 C0 04 56 50 8B 82 A0 00 00 00 FF D0");
            if (!savePropertyPattern.empty())
                SaveDiamondPropertyHook = safetyhook::create_inline(savePropertyPattern.get_first(), SaveDiamondProperty);

            // CEconomyComponent's constructor, hooked at +0x1A past both vtable stores with ESI
            // still the object. A new component always arrives before the save's count lands in it.
            auto economyCtorPattern = dunia_pattern("56 8B F1 E8 ? ? ? ? 33 C0 89 46 10 C7 06 ? ? ? ? C7 46 04 ? ? ? ? 89 46 14 89 46 18 89 46 1C 89 46 20 89 46 24 89 46 28 8B C6 5E C3");
            if (!economyCtorPattern.empty())
            {
                static auto EconomyCtorHook = safetyhook::create_mid(economyCtorPattern.get_first(nEconomyCtorVTableSet), [](SafetyHookContext& regs)
                {
                    nGranted = 0;
                    NoteEconomy(reinterpret_cast<void*>(regs.esi));
                });
            }

            // CFCXCountersComponentPlayerSP's constructor, hooked at +0x15 past the vtable stores
            // with ESI still the object. Three siblings share the layout; this is the campaign one.
            auto countersCtorPattern = dunia_pattern("F3 0F 11 8E 10 01 00 00 C7 06 ? ? ? ? C7 46 04 ? ? ? ? F3 0F 11 86 F4 00 00 00");
            if (!countersCtorPattern.empty())
            {
                static auto CountersCtorHook = safetyhook::create_mid(countersCtorPattern.get_first(nCountersCtorVTableSet), [](SafetyHookContext& regs)
                {
                    pCounters = reinterpret_cast<void*>(regs.esi);
                    pCountersVTable = *reinterpret_cast<void**>(regs.esi);
                });
            }

            // The three syringe spend sites, whose return addresses tell ItemConsume a syringe from
            // a magazine. Three matches, so taken by index rather than through get_first.
            auto syringeSitePattern = dunia_pattern("3B C1 75 04 33 C9 EB 02 8B 08 6A 01 E8");
            for (size_t i = 0; i < syringeSitePattern.size() && nSyringeConsumeSiteCount < nSyringeConsumeSites; i++)
            {
                pSyringeConsumeSite[nSyringeConsumeSiteCount++] =
                    reinterpret_cast<uintptr_t>(syringeSitePattern.get(i).get<void>(nSyringeConsumeReturn));
            }

            // The shared item decrementer, hooked only once the sites above are known.
            auto itemConsumePattern = dunia_pattern("8B 54 24 04 85 D2 7F 05 33 C0 C2 04 00 80 79 1C 00 75 0E A1 ? ? ? ? 83 B8 98 00 00 00 00 76 05");
            if (!itemConsumePattern.empty() && nSyringeConsumeSiteCount != 0)
                ItemConsumeHook = safetyhook::create_inline(itemConsumePattern.get_first(), ItemConsume);

            // CVehicle::GetCurrentVehicle. The trailing 82 A7 1B FD is the fact CRC 0xFD1BA782;
            // without it the prologue is ordinary and the pattern is not unique.
            auto currentVehiclePattern = dunia_pattern("83 EC 08 8B 4C 24 0C 8B 01 8B 50 7C 53 FF D2 83 CB FF B9 01 00 00 00 84 0D ? ? ? ? 89 5C 24 04 89 5C 24 08 75 10 09 0D ? ? ? ? C7 05 ? ? ? ? 82 A7 1B FD");
            if (!currentVehiclePattern.empty())
                GetCurrentVehicle = reinterpret_cast<GetCurrentVehicle_t>(currentVehiclePattern.get_first());

            auto flushJobPattern = dunia_pattern("56 8B F1 8B 86 C8 00 00 00 85 C0 74 26 8B 80 C4 00 00 00 85 C0 74 1C 8B 0D ? ? ? ? 50 E8");
            if (!flushJobPattern.empty())
                FlushEntityJob = reinterpret_cast<FlushEntityJob_t>(flushJobPattern.get_first());

            // GetComponent<CVehiclePhysComponent>. The registrar call displacement is left
            // unwildcarded, or the bytes match 69 other getters; build specific, hence two patterns.
            auto vehiclePhysicsPattern = dunia_pattern(
                "83 3D ? ? ? ? 00 56 8B F1 75 07 33 C9 E8 3D D6 FD FF 68 ? ? ? ? 8B CE E8 ? ? ? ? 5E C3",
                "83 3D ? ? ? ? 00 56 8B F1 75 07 33 C9 E8 0D DE FD FF 68 5C D9 F2 10 8B CE E8 ? ? ? ? 5E C3");
            if (!vehiclePhysicsPattern.empty())
                GetVehiclePhysics = reinterpret_cast<GetVehiclePhysics_t>(vehiclePhysicsPattern.get_first());

            // CVehiclePhysComponent::ApplyHealthDamage, anchored on its gate tests at +0x102 and
            // +0x106. Both bytes are recomputed every physics step, so clearing them does not hold.
            auto vehicleDamagePattern = dunia_pattern("80 B9 02 01 00 00 00 0F 57 C9 74 2C 80 B9 06 01 00 00 00");
            if (!vehicleDamagePattern.empty())
                VehicleHealthDamageHook = safetyhook::create_inline(vehicleDamagePattern.get_first(), VehicleHealthDamage);

            // CVehiclePhysComponent::ApplyStimToParts.
            auto vehicleStimPattern = dunia_pattern("55 8B EC 83 E4 F0 81 EC B4 00 00 00 53 56 57 8B 7D 08 85 FF 8B D9 0F 84 AA 00 00 00");
            if (!vehicleStimPattern.empty())
                VehicleStimPartsHook = safetyhook::create_inline(vehicleStimPattern.get_first(), VehicleStimParts);

            // CPlayer::GetLocalPlayer, and the two camera manager entry points the mode switch needs.
            auto localPlayerPattern = dunia_pattern("A1 ? ? ? ? 83 78 08 00 75 03 33 C0 C3 8B 40 04 8B 00 C3");
            auto setCameraPattern = dunia_pattern("51 53 55 56 8B F1 80 7E 14 00 57 0F 85");
            auto getCameraPattern = dunia_pattern("8B 41 10 85 C0 7C 20 3B 41 1C 73 1B 51 8B 49 18 8D 04 40");

            if (!localPlayerPattern.empty() && !setCameraPattern.empty() && !getCameraPattern.empty())
            {
                GetLocalPlayer = reinterpret_cast<GetLocalPlayer_t>(localPlayerPattern.get_first());
                SetActiveCameraByName = reinterpret_cast<SetActiveCameraByName_t>(setCameraPattern.get_first());
                GetActiveCamera = reinterpret_cast<GetActiveCamera_t>(getCameraPattern.get_first());
            }

            // The manager's resolve-and-set-focus sequence, read not hooked: the ref world global
            // at +0x0F and the id resolver called at +0x13.
            auto focusResolvePattern = dunia_pattern("8B 55 0C 8B 45 08 52 50 8D 4C 24 4C 51 8B 0D ? ? ? ? E8 ? ? ? ? 8B 7C 24 44 83 7F 0C 00 74 12 51 8B C4 89 38 83 47 08 01 8B 13 8B 42 78 8B CB FF D0");
            if (!focusResolvePattern.empty())
            {
                auto pMatch = reinterpret_cast<uintptr_t>(focusResolvePattern.get_first());

                ppRefWorld = *reinterpret_cast<void***>(pMatch + nRefWorldGlobal);

                // Past the opcode, over the displacement, to what it points at.
                auto pCall = pMatch + nEntityRefFromIdCall;
                EntityRefFromId = reinterpret_cast<EntityRefFromId_t>(pCall + 5 + *reinterpret_cast<int32_t*>(pCall + 1));
            }

            auto releaseRefPattern = dunia_pattern("51 56 8B F1 E8 ? ? ? ? 56 8D 4C 24 08 51 8B C8 E8 ? ? ? ? E8 ? ? ? ? 8B 54 24 04 3B 50 10");
            if (!releaseRefPattern.empty())
                ReleaseEntityRef = reinterpret_cast<ReleaseEntityRef_t>(releaseRefPattern.get_first());

            auto freeRefPattern = dunia_pattern("56 8B 74 24 08 85 F6 74 20 56 E8 ? ? ? ? 83 C4 04 84 C0 56 74 0A");
            if (!freeRefPattern.empty())
                FreeEntityRef = reinterpret_cast<FreeEntityRef_t>(freeRefPattern.get_first());

            // The physics tag, its lazy registrar and the fetch, from the one place the engine does
            // all three in a row: the ghost camera's activate handler.
            auto physicsTagPattern = dunia_pattern("83 3D ? ? ? ? 00 75 07 33 C9 E8 ? ? ? ? 68 ? ? ? ? 8B CE E8 ? ? ? ? 85 ED 8B F8 75 0D 85 FF 75 09");
            if (!physicsTagPattern.empty())
            {
                auto pMatch = reinterpret_cast<uintptr_t>(physicsTagPattern.get_first());

                pPhysicsTagFlag = *reinterpret_cast<int32_t**>(pMatch + nPhysicsTagFlag);
                pPhysicsTag = *reinterpret_cast<void**>(pMatch + nPhysicsTagValue);

                auto pRegister = pMatch + nPhysicsTagRegisterCall;
                RegisterPhysicsTag = reinterpret_cast<RegisterPhysicsTag_t>(pRegister + 5 + *reinterpret_cast<int32_t*>(pRegister + 1));

                auto pFetch = pMatch + nGetComponentCall;
                GetComponentByTag = reinterpret_cast<GetComponentByTag_t>(pFetch + 5 + *reinterpret_cast<int32_t*>(pFetch + 1));
            }

            // CEntity::SetPosition, patternable only because SetEuler sits behind it.
            auto setPositionPattern = dunia_pattern("6A 00 8D 44 24 08 50 E8 ? ? ? ? C2 0C 00 CC 6A 00 8D 44 24 08 50 E8 ? ? ? ? C2 0C 00");
            if (!setPositionPattern.empty())
            {
                SetEntityPosition = reinterpret_cast<SetEntityPosition_t>(setPositionPattern.get_first());
                SetEntityEuler = reinterpret_cast<SetEntityEuler_t>(setPositionPattern.get_first(0x10));
            }

            // The render camera accessor; noclip's starting pitch comes off it.
            auto renderCameraPattern = dunia_pattern("83 C1 14 6A 01 51 B9 ? ? ? ? E8 ? ? ? ? C3");
            if (!renderCameraPattern.empty())
                GetRenderCamera = reinterpret_cast<GetRenderCamera_t>(renderCameraPattern.get_first());

            // The signal dispatcher; opens with a five byte MOV EAX,[imm32], nothing to relocate.
            auto gameSignalPattern = dunia_pattern("A1 ? ? ? ? 83 EC 50 A8 01 53 55 56 57 8B F9");
            if (!gameSignalPattern.empty())
                GameSignalHook = safetyhook::create_inline(gameSignalPattern.get_first(), GameSignal);

            // CCameraFreeComponent::Update.
            auto freeCameraPattern = dunia_pattern("83 EC 28 53 55 56 57 8B 3D ? ? ? ? 8B F1 8D 44 24 14 8D 4E FC 50");
            if (!freeCameraPattern.empty())
                FreeCameraUpdateHook = safetyhook::create_inline(freeCameraPattern.get_first(), FreeCameraUpdate);

            // The pawn's fall update, __thiscall with ESI the state block. Collision off takes the
            // ground with it, so clear the flag and jump to the not-falling tail at +0x303.
            auto fallUpdatePattern = dunia_pattern("80 BE 9B 04 00 00 00 F3 0F 10 05 ? ? ? ? C6 44 24 12 00 F3 0F 11 44 24 18 0F 85 ? ? ? ? F6 86 D4 02 00 00 20");
            if (!fallUpdatePattern.empty())
            {
                static auto nFallUpdateRejoin = reinterpret_cast<uintptr_t>(fallUpdatePattern.get_first(nFallUpdateTail));

                static auto FallUpdateHook = safetyhook::create_mid(fallUpdatePattern.get_first(), [](SafetyHookContext& regs)
                {
                    // The player's block only. Every pawn in the level runs this function.
                    auto nBlock = static_cast<uintptr_t>(regs.esi);
                    if (eCameraMode != CameraMode::Noclip || nBlock == 0 || nBlock != nNoclipStateBlock)
                        return;

                    *reinterpret_cast<uint8_t*>(nBlock + nPawnFalling) = 0;
                    *reinterpret_cast<int32_t*>(nBlock + nPawnFallFrames) = 0;

                    regs.eip = nFallUpdateRejoin;
                });
            }

            // CPawnInputListener::Update, the module's clock. +0x16 needs gameplay input enabled
            // and a pawn alive, and lands on a call, so XMM is caller-saved. Resolved in dunia.ixx.
            auto* pInputPass = dunia_input_pass();
            if (pInputPass != nullptr)
            {
                static auto InputPassHook = safetyhook::create_mid(static_cast<uint8_t*>(pInputPass) + nInputPassLivePawn, [](SafetyHookContext& regs)
                {
                    // ESI is the listener, ECX the pawn the relocated instruction just fetched.
                    Tick(static_cast<uintptr_t>(regs.esi), static_cast<uintptr_t>(regs.ecx));
                });
            }

            JackalFix::onIniFileChange() += []()
            {
                ReadSettings();
            };

            // State only: shutdown runs under the loader lock, so a camera switch buys nothing.
            JackalFix::onShutdownEvent() += []()
            {
                ResetCameraState();

                pVehiclePhysics = nullptr;
                pVehicleRefBlock = nullptr;
                pVehicleEntity = nullptr;
            };
        };
    }
} Debug;
