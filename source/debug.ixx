/*
  Debug and testing aids.

  GameProfile
  Four dwords on the settings object, tested by their consumers with nothing in front of them.
  Re-applied every frame: the spectator path clears GodMode and a profile reload on a map transition
  writes the file's value back.

      GodMode  +0x94    UnlimitedAmmo  +0x98    UnlimitedReliability  +0x9C    AllWeaponsUnlock  +0xA0

  AllWeaponsUnlock only bypasses the per-weapon unlock list. The other map's weapons are hidden by
  two act gates ahead of it in CWeaponBazaar::IsWeaponUnlocked, comparing entry->act against 1 and 2.
  The field only ever holds 0, 1 or 2, so both immediates are patched to 0xFF. The rank,
  prerequisite and already-owned checks below are untouched.

  Diamonds
  One int32 at CEconomyComponent+0x10. Live count topped up, nGranted tracks how much is ours,
  spending retires the grant first, and the save hook writes live - nGranted so granted diamonds
  never reach the disc.

  Freecam
  Activates cameras.Camera.Free by name. A self contained fly camera that also pushes an exclusive
  free_camera input mapping and parks the pawn. The manager only hands a camera its focus on the miss
  path that instantiates it, so later activations call that vtable slot directly. Its Locked byte
  silently no-ops the switch, so it is cleared and put back.

  Noclip
  cameras.Camera.Ghost is rejected: it drives the player's transform after the animation pass, which
  leaves the viewmodel a frame behind. So the gameplay camera is kept and the player moved underneath
  it. Physics off, position integrated in the input pass ahead of animation, yaw integrated by hand
  since the character controller that applies it is part of the physics switched off.

  Hook placement
  The clock is a mid hook on CPawnInputListener::Update, past its check that gameplay input is
  enabled. A camera update will not do: activating a camera by name there mutates the manager's array
  mid-iteration. It and CIntProperty::Serialise are hooked mid-function rather than at the entry,
  because lookback.ixx and renderconfig.ixx own those prologues and two inline hooks on one prologue
  resolve by static initialisation order.
*/

module;

#include <common.hxx>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <string_view>

export module debug;

import common;
import dunia;
import settings;
import inputdevice;

// ------------------------------------------------------------------------------------------------
// GameProfile object.

static constexpr ptrdiff_t nProfileGodMode = 0x94;
static constexpr ptrdiff_t nProfileUnlimitedAmmo = 0x98;
static constexpr ptrdiff_t nProfileAllWeaponsUnlock = 0xA0;

// Several consumers compare the field unsigned, so a negative value reads as false.
static constexpr int32_t nCheatOn = 1;
static constexpr int32_t nCheatOff = 0;

// ------------------------------------------------------------------------------------------------
// CEconomyComponent and the diamond count property descriptors.

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

// CPawnInputListener's look accumulators. Y is horizontal, X vertical.
static constexpr ptrdiff_t nListenerLookX = 0x10;
static constexpr ptrdiff_t nListenerLookY = 0x14;

// The HUD's mirrors: same name and descriptor type, hence testing offset as well as hash.
static constexpr int32_t nHudDiamondCount = 0x2BC;
static constexpr int32_t nHudLastDiamondCount = 0x2C8;

// ------------------------------------------------------------------------------------------------
// Cameras.

static constexpr ptrdiff_t nPlayerScene = 0x04;
static constexpr ptrdiff_t nSceneCameraManager = 0xB8;
static constexpr ptrdiff_t nManagerFocusLow = 0x08;
static constexpr ptrdiff_t nManagerFocusHigh = 0x0C;
static constexpr ptrdiff_t nManagerLocked = 0x14;

static constexpr size_t nCameraSetFocusSlot = 0x78 / sizeof(void*);

// An entity ref holder: refcount, then the entity it stands for.
static constexpr ptrdiff_t nEntityRefCount = 0x08;
static constexpr ptrdiff_t nEntityRefEntity = 0x0C;

// Into the pattern spanning the manager's resolve-and-set-focus sequence.
static constexpr ptrdiff_t nRefWorldGlobal = 0x0F;
static constexpr ptrdiff_t nEntityRefFromIdCall = 0x13;

// CCameraFreeComponent, which CCameraGhostComponent derives from without adding fields. Move axes
// are in the camera's local frame; look values are rates, scaled by 180 degrees a second per unit.
static constexpr ptrdiff_t nCameraMoveForward = 0xB4;
static constexpr ptrdiff_t nCameraMoveStrafe = 0xB8;
static constexpr ptrdiff_t nCameraMoveVertical = 0xBC;
static constexpr ptrdiff_t nCameraLookYaw = 0xC0;
static constexpr ptrdiff_t nCameraLookPitch = 0xC4;
static constexpr ptrdiff_t nCameraSpeed = 0xC8;
static constexpr ptrdiff_t nCameraSpeedAdjust = 0xCC;

// Metres a second on the baseline step.
static constexpr float fFreecamBaseSpeed = 12.0f;

static constexpr const char* pCameraGameplay = "Cameras.Camera.First";
static constexpr const char* pCameraFree = "Cameras.Camera.Free";

// 4x4 row-major: rows 0 to 2 the basis, row 3 the translation.
static constexpr ptrdiff_t nEntityMatrix = 0x30;

// The enable slot the ghost camera's activate handler zeroes.
static constexpr size_t nPhysicsEnabledSlot = 0xB4 / sizeof(void*);

// Into the pattern spanning the ghost camera's tag registration and component fetch.
static constexpr ptrdiff_t nPhysicsTagFlag = 0x02;
static constexpr ptrdiff_t nPhysicsTagRegisterCall = 0x0B;
static constexpr ptrdiff_t nPhysicsTagValue = 0x11;
static constexpr ptrdiff_t nGetComponentCall = 0x17;

// Two halves of the block off pawn+0x10, requested and current. Both carry a flags byte.
static constexpr ptrdiff_t nPawnStateBlock = 0x10;
static constexpr ptrdiff_t nPawnRequestedState = 0x140;
static constexpr ptrdiff_t nPawnCurrentState = 0x2D0;

static constexpr ptrdiff_t nStateFlags = 0x04;
static constexpr uint8_t nSprintFlag = 0x40;

// Angle triples here are (pitch, roll, yaw).
static constexpr size_t nAnglePitch = 0;

// The engine integrates look as angle += delta * frameTime * 180 degrees, negated for yaw.
static constexpr float fLookRadiansPerUnit = 3.14159265f;

// Read straight from XInput; inputdevice.ixx already mid-hooks the engine's poll. Movement and the
// speed step only; look arrives on the pawn's accumulator whichever device is in use.
struct XInputGamepad
{
    uint16_t nButtons;
    uint8_t nLeftTrigger;
    uint8_t nRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};

struct XInputState
{
    uint32_t nPacket;
    XInputGamepad Gamepad;
};

using XInputGetState_t = uint32_t(WINAPI*)(uint32_t nUser, XInputState* pState);

static XInputGetState_t XInputGetStateFn = nullptr;

// XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE.
static constexpr float fThumbDeadzone = 7849.0f;
static constexpr float fThumbRange = 32767.0f;

// The sprint button, free to reuse: noclip clears the sprint request.
static constexpr uint16_t nPadSpeedCycle = 0x0040;

// Signals noclip refuses, by the CRC32 the dispatcher identifies them with.
static constexpr uint32_t nSignalPauseMenu = 0x04127107; // "show_pausemenu"
static constexpr uint32_t nSignalQuickSave = 0xEFEF8B90; // "quicksave"
static constexpr uint32_t nSignalQuickLoad = 0x9F8F5553; // "quickload"

static constexpr ptrdiff_t nRenderCameraEuler = 0x6C;

// Just short of straight up and down. With physics off, nothing else holds the pitch.
static constexpr float fLookPitchLimit = 1.55f;

// Flip if looking up flies down.
static constexpr float fNoclipPitchSign = 1.0f;

static constexpr size_t nLookAccumulatorPitch = 0;
static constexpr size_t nLookAccumulatorYaw = 1;

// Metres a second at the baseline step. The engine's own free camera starts at 5.
static constexpr float fNoclipBaseSpeed = 12.0f;

// Fixed bindings. Only the two mode keys are configurable.
static constexpr int nKeyForward = 'W';
static constexpr int nKeyBack = 'S';
static constexpr int nKeyStrafeLeft = 'A';
static constexpr int nKeyStrafeRight = 'D';
static constexpr int nKeyUp = VK_SPACE;
static constexpr int nKeyDown = VK_LCONTROL;
static constexpr int nKeySpeedCycle = VK_LSHIFT;
static constexpr int nKeyLookLeft = VK_LEFT;
static constexpr int nKeyLookRight = VK_RIGHT;
static constexpr int nKeyLookUp = VK_UP;
static constexpr int nKeyLookDown = VK_DOWN;

// A whole unit is a half turn a second, far too fast for a key.
static constexpr float fLookRate = 0.35f;

static constexpr float fMouseLookGain = 1.0f;

static constexpr float fSpeedSteps[]{ 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f };

// Noclip stops at 16x. Past that the player outruns the streaming.
static constexpr size_t nNoclipSpeedSteps = 5;

// ------------------------------------------------------------------------------------------------
// Engine functions.

// Null outside of a session.
using GetLocalPlayer_t = void* (__cdecl*)();

// __thiscall on the manager. Plain string, not a std::string, matched case insensitively.
using SetActiveCameraByName_t = void(__fastcall*)(void* pManager, void* pEdx, const char* pName, int32_t bNotify);

using GetActiveCamera_t = void* (__fastcall*)(void* pManager);

// __thiscall on the ref world, callee cleans twelve bytes. Caller owns a reference on the holder.
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

using GetComponentByTag_t = void* (__fastcall*)(void* pEntity, void* pEdx, void* pTag);

// __thiscall on an entity, three floats by value, callee cleans twelve bytes. SetEuler sits directly
// behind SetPosition and takes the same shape.
using SetEntityPosition_t = void(__fastcall*)(void* pEntity, void* pEdx, float fX, float fY, float fZ);
using SetEntityEuler_t = void(__fastcall*)(void* pEntity, void* pEdx, float fPitch, float fRoll, float fYaw);

using GetRenderCamera_t = void* (__fastcall*)(void* pCamera);

// Vtable slot 0xB4 on the physics component.
using SetPhysicsEnabled_t = void(__fastcall*)(void* pPhysics, void* pEdx, int32_t bEnabled);

static RegisterPhysicsTag_t RegisterPhysicsTag = nullptr;
static GetComponentByTag_t GetComponentByTag = nullptr;
static SetEntityPosition_t SetEntityPosition = nullptr;
static SetEntityEuler_t SetEntityEuler = nullptr;
static GetRenderCamera_t GetRenderCamera = nullptr;

// Read out of the instruction that loads it, so it moves with the image.
static void** ppRefWorld = nullptr;

static void* pPhysicsTag = nullptr;
static int32_t* pPhysicsTagFlag = nullptr;

static void** ppGameProfile = nullptr;

// ------------------------------------------------------------------------------------------------
// State.

enum class CameraMode
{
    None,
    Noclip,
    Freecam,
};

static CameraMode eCameraMode = CameraMode::None;

// The camera the switch produced, so a camera the game activated is never fed our input.
static void* pDebugCamera = nullptr;

static void* pDebugCameraManager = nullptr;
static uint8_t nSavedLocked = 0;

static void* pNoclipEntityRef = nullptr;
static void* pNoclipPhysics = nullptr;

// Wall clock for the frame delta, since the input pass does not carry one.
static int64_t nNoclipLastCounter = 0;

static float fNoclipYaw = 0.0f;
static float fNoclipPitch = 0.0f;

// Set while the camera's axes hold values this module wrote, so they zero exactly once on release.
// Latched apart so keyboard movement does not zero the stick.
static bool bFedCameraMove = false;
static bool bFedCameraLook = false;

static size_t nSpeedStep = 0;

static size_t SpeedStepCount()
{
    return (eCameraMode == CameraMode::Noclip)
        ? nNoclipSpeedSteps
        : (sizeof(fSpeedSteps) / sizeof(fSpeedSteps[0]));
}

// Look input since the camera last consumed it, in the camera's own units.
static float fMouseLookX = 0.0f;
static float fMouseLookY = 0.0f;

// The player's economy component and the vtable it carried when first seen. It dies with the
// player while this module's clock ticks on, hence the liveness test.
static void* pEconomy = nullptr;
static void* pEconomyVTable = nullptr;

// Granted diamonds still in the wallet.
static int32_t nGranted = 0;

// Mirrored out of the ini so the per-frame path is not reading a variant.
static int32_t nDiamondTarget = 0;

static bool bInvincibility = false;
static bool bInfiniteAmmo = false;
static bool bUnlockAllWeapons = false;
static bool bNoclipEnabled = false;
static bool bFreecamEnabled = false;

static int nNoclipKey = 0;
static int nFreecamKey = 0;

// ------------------------------------------------------------------------------------------------
// Key names.

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

    if (szUpper.size() == 1 && ((szUpper[0] >= 'A' && szUpper[0] <= 'Z') || (szUpper[0] >= '0' && szUpper[0] <= '9')))
        return szUpper[0];

    auto nBase = (szUpper.rfind("0X", 0) == 0) ? 16 : 10;
    auto nParsed = std::strtol(szUpper.c_str() + (nBase == 16 ? 2 : 0), nullptr, nBase);

    return (nParsed > 0 && nParsed < 256) ? static_cast<int>(nParsed) : 0;
}

// Or the keys would fire while the player is alt-tabbed and typing somewhere else.
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

static bool KeyPressed(int nKey, bool& bLatch)
{
    auto bDown = KeyDown(nKey);
    auto bEdge = bDown && !bLatch;
    bLatch = bDown;

    return bEdge;
}

static float ThumbAxis(int16_t sValue)
{
    auto fValue = static_cast<float>(sValue);
    if (fValue > -fThumbDeadzone && fValue < fThumbDeadzone)
        return 0.0f;

    auto fScaled = (fValue - (fValue > 0.0f ? fThumbDeadzone : -fThumbDeadzone)) / (fThumbRange - fThumbDeadzone);

    return std::clamp(fScaled, -1.0f, 1.0f);
}

static bool ReadPad(XInputGamepad& Pad)
{
    if (XInputGetStateFn == nullptr)
    {
        static const wchar_t* pModules[]{ L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll" };

        for (auto pModule : pModules)
        {
            auto hModule = GetModuleHandleW(pModule);
            if (hModule == nullptr)
                hModule = LoadLibraryW(pModule);

            if (hModule != nullptr)
            {
                XInputGetStateFn = reinterpret_cast<XInputGetState_t>(GetProcAddress(hModule, "XInputGetState"));
                if (XInputGetStateFn != nullptr)
                    break;
            }
        }

        if (XInputGetStateFn == nullptr)
            return false;
    }

    for (uint32_t nUser = 0; nUser < 4; nUser++)
    {
        XInputState State{};
        if (XInputGetStateFn(nUser, &State) == ERROR_SUCCESS)
        {
            Pad = State.Gamepad;
            return true;
        }
    }

    return false;
}

static bool PadPressed(uint16_t nButtons, uint16_t nButton, bool& bLatch)
{
    auto bDown = (nButtons & nButton) != 0;
    auto bEdge = bDown && !bLatch;
    bLatch = bDown;

    return bEdge;
}

// ------------------------------------------------------------------------------------------------
// GameProfile fields.

static void ApplyProfileFlags()
{
    if (ppGameProfile == nullptr)
        return;

    auto pProfile = reinterpret_cast<uintptr_t>(*ppGameProfile);
    if (pProfile == 0)
        return;

    *reinterpret_cast<int32_t*>(pProfile + nProfileGodMode) = bInvincibility ? nCheatOn : nCheatOff;
    *reinterpret_cast<int32_t*>(pProfile + nProfileUnlimitedAmmo) = bInfiniteAmmo ? nCheatOn : nCheatOff;
    *reinterpret_cast<int32_t*>(pProfile + nProfileAllWeaponsUnlock) = bUnlockAllWeapons ? nCheatOn : nCheatOff;
}

// ------------------------------------------------------------------------------------------------
// Diamonds.

static int32_t* LiveDiamonds()
{
    if (pEconomy == nullptr)
        return nullptr;

    return reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pEconomy) + nEconomyDiamondCount);
}

static bool EconomyIsLive()
{
    if (pEconomy == nullptr || pEconomyVTable == nullptr)
        return false;

    if (GetLocalPlayer == nullptr || GetLocalPlayer() == nullptr)
        return false;

    return *reinterpret_cast<void**>(pEconomy) == pEconomyVTable;
}

// Hands the grant back to the wallet. Abandoning it would promote granted diamonds to real progress.
static void RetireGrant()
{
    if (nGranted <= 0)
        return;

    if (EconomyIsLive())
    {
        auto pLive = LiveDiamonds();
        *pLive = (*pLive > nGranted) ? (*pLive - nGranted) : 0;
    }

    nGranted = 0;
}

static void NoteEconomy(void* pComponent)
{
    if (pComponent == nullptr || pComponent == pEconomy)
        return;

    RetireGrant();

    pEconomy = pComponent;

    if (pEconomyVTable == nullptr)
        pEconomyVTable = *reinterpret_cast<void**>(pComponent);
}

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

static void ApplyDiamonds()
{
    // Not dropped on failure: the constructor hook re-points it on the next load.
    if (!EconomyIsLive())
        return;

    auto pLive = LiveDiamonds();

    if (nDiamondTarget > 0)
    {
        if (*pLive < nDiamondTarget)
        {
            nGranted += nDiamondTarget - *pLive;
            *pLive = nDiamondTarget;
        }
        else if (nGranted > 0 && *pLive > nDiamondTarget)
        {
            // The ini was lowered. Hand back only the part of the grant above the new target.
            auto nBack = *pLive - nDiamondTarget;
            if (nBack > nGranted)
                nBack = nGranted;

            *pLive -= nBack;
            nGranted -= nBack;
        }

        // A count lowered outside these hooks would leave the grant overstated.
        if (nGranted > *pLive)
            nGranted = *pLive;
        if (nGranted < 0)
            nGranted = 0;

        return;
    }

    RetireGrant();
}

// ------------------------------------------------------------------------------------------------
// Camera switching.

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

// The manager's focus (the player) as a ref holder. Caller must pass it to ReleaseFocusEntity.
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

// Puts the camera back on the player. The manager only does this on the activation that makes it.
static void SnapCameraToFocus(void* pManager, void* pCamera)
{
    if (pCamera == nullptr)
        return;

    auto pEntityRef = AcquireFocusEntity(pManager);
    if (pEntityRef == nullptr)
        return;

    if (FocusEntity(pEntityRef) != nullptr)
    {
        // Passed by value: this reference is the one the callee drops.
        *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pEntityRef) + nEntityRefCount) += 1;

        auto ppVTable = *reinterpret_cast<void***>(pCamera);
        reinterpret_cast<SetFocus_t>(ppVTable[nCameraSetFocusSlot])(pCamera, nullptr, pEntityRef);
    }

    ReleaseFocusEntity(pEntityRef);
}

// ------------------------------------------------------------------------------------------------
// Noclip.

static void SetPhysicsEnabled(void* pPhysics, bool bEnabled)
{
    if (pPhysics == nullptr)
        return;

    auto ppVTable = *reinterpret_cast<void***>(pPhysics);
    reinterpret_cast<SetPhysicsEnabled_t>(ppVTable[nPhysicsEnabledSlot])(pPhysics, nullptr, bEnabled ? 1 : 0);
}

static void LeaveNoclip()
{
    if (pNoclipEntityRef == nullptr)
        return;

    // Collision back before the reference goes, so the last thing touched is still alive.
    SetPhysicsEnabled(pNoclipPhysics, true);

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

    // Seeded from the body's facing. Local +Y is forward, hence the quarter turn.
    auto pMatrix = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(pEntity) + nEntityMatrix);
    fNoclipYaw = std::atan2(pMatrix[5], pMatrix[4]) - 1.57079633f;
    fNoclipPitch = 0.0f;

    // Pitch only exists on the camera.
    if (GetActiveCamera != nullptr && GetRenderCamera != nullptr)
    {
        auto pCamera = GetActiveCamera(ResolveCameraManager());
        if (pCamera != nullptr)
        {
            auto nRenderCamera = reinterpret_cast<uintptr_t>(GetRenderCamera(pCamera));
            if (nRenderCamera != 0)
                fNoclipPitch = *reinterpret_cast<float*>(nRenderCamera + nRenderCameraEuler + nAnglePitch * sizeof(float));
        }
    }

    pNoclipEntityRef = pEntityRef;
    pNoclipPhysics = pPhysics;

    return true;
}

// Runs from the input pass, ahead of the animation that places the arms.
static void ApplyNoclip(uintptr_t nListener, uintptr_t nPawn, float fDelta)
{
    auto pEntity = FocusEntity(pNoclipEntityRef);
    if (pEntity == nullptr)
        return;

    // Sprint goes nowhere, but the request still reaches the animation layer. Clear both halves.
    auto nState = (nPawn != 0) ? *reinterpret_cast<uintptr_t*>(nPawn + nPawnStateBlock) : 0;
    if (nState != 0)
    {
        *reinterpret_cast<uint8_t*>(nState + nPawnRequestedState + nStateFlags) &= static_cast<uint8_t>(~nSprintFlag);
        *reinterpret_cast<uint8_t*>(nState + nPawnCurrentState + nStateFlags) &= static_cast<uint8_t>(~nSprintFlag);
    }

    // The character controller that turns the body is part of the physics switched off above, so
    // yaw is integrated here. Pitch only feeds the flight direction.
    if (nListener != 0 && fDelta > 0.0f)
    {
        auto pAccumulator = reinterpret_cast<const float*>(nListener + nListenerLookX);

        fNoclipYaw -= pAccumulator[nLookAccumulatorYaw] * fDelta * fLookRadiansPerUnit;
        fNoclipPitch += pAccumulator[nLookAccumulatorPitch] * fDelta * fLookRadiansPerUnit;

        // The controller also held the pitch short of vertical.
        fNoclipPitch = std::clamp(fNoclipPitch, -fLookPitchLimit, fLookPitchLimit);
    }

    // (pitch, roll, yaw). The body stays upright; pitching it takes the head bone and arms with it.
    if (SetEntityEuler != nullptr)
        SetEntityEuler(pEntity, nullptr, 0.0f, 0.0f, fNoclipYaw);

    if (fDelta <= 0.0f || !HasFocus())
        return;

    auto fStrafe = (KeyDown(nKeyStrafeRight) ? 1.0f : 0.0f) - (KeyDown(nKeyStrafeLeft) ? 1.0f : 0.0f);
    auto fForward = (KeyDown(nKeyForward) ? 1.0f : 0.0f) - (KeyDown(nKeyBack) ? 1.0f : 0.0f);
    auto fVertical = (KeyDown(nKeyUp) ? 1.0f : 0.0f) - (KeyDown(nKeyDown) ? 1.0f : 0.0f);

    // The pad adds to the keyboard. Height stays on keys, since the triggers are aim and fire.
    XInputGamepad Pad{};
    if (ReadPad(Pad))
    {
        fStrafe += ThumbAxis(Pad.sThumbLX);
        fForward += ThumbAxis(Pad.sThumbLY);
    }

    fStrafe = std::clamp(fStrafe, -1.0f, 1.0f);
    fForward = std::clamp(fForward, -1.0f, 1.0f);
    fVertical = std::clamp(fVertical, -1.0f, 1.0f);

    if (fStrafe == 0.0f && fForward == 0.0f && fVertical == 0.0f)
        return;

    auto fStep = fNoclipBaseSpeed * fSpeedSteps[nSpeedStep] * fDelta;

    // The entity's own basis: X strafes, Y forward, Z up. SetEuler rebuilt it, so the yaw is in.
    auto pMatrix = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(pEntity) + nEntityMatrix);

    auto pRight = pMatrix;
    auto pAhead = pMatrix + 4;
    auto pUp = pMatrix + 8;

    // The body is upright, so the flight pitch is applied here.
    auto fCos = std::cos(fNoclipPitch);
    auto fSin = std::sin(fNoclipPitch) * fNoclipPitchSign;

    float fMove[3]{};
    for (auto i = 0; i < 3; i++)
    {
        auto fAheadTilted = pAhead[i] * fCos + pUp[i] * fSin;
        fMove[i] = (fStrafe * pRight[i] + fForward * fAheadTilted + fVertical * pUp[i]) * fStep;
    }

    SetEntityPosition(pEntity, nullptr,
                      pMatrix[12] + fMove[0],
                      pMatrix[13] + fMove[1],
                      pMatrix[14] + fMove[2]);
}

// A level load destroys the manager and a cutscene may take the camera, so ask the engine first.
static bool DebugCameraStillOurs()
{
    if (eCameraMode == CameraMode::None || GetActiveCamera == nullptr)
        return false;

    auto pManager = ResolveCameraManager();
    if (pManager == nullptr || pManager != pDebugCameraManager)
        return false;

    return GetActiveCamera(pManager) == pDebugCamera;
}

// Puts the Locked byte back if its manager is still live. Every exit path ends here.
static void ForgetCameraMode()
{
    if (pDebugCameraManager != nullptr && pDebugCameraManager == ResolveCameraManager())
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pDebugCameraManager) + nManagerLocked) = nSavedLocked;

    eCameraMode = CameraMode::None;
    pDebugCamera = nullptr;
    pDebugCameraManager = nullptr;
    nSavedLocked = 0;
    bFedCameraMove = false;
    bFedCameraLook = false;
    fMouseLookX = 0.0f;
    fMouseLookY = 0.0f;
}

static void LeaveCameraMode()
{
    if (eCameraMode == CameraMode::None)
        return;

    if (DebugCameraStillOurs() && SetActiveCameraByName != nullptr)
    {
        ClearCameraAxes(pDebugCamera);
        SetActiveCameraByName(pDebugCameraManager, nullptr, pCameraGameplay, 1);

        // If the gameplay camera did not take, keep the mode, or the key can no longer exit it.
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

    // The camera keeps its axes and position between activations.
    ClearCameraAxes(pCamera);
    SnapCameraToFocus(pManager, pCamera);

    eCameraMode = CameraMode::Freecam;
    pDebugCamera = pCamera;
    pDebugCameraManager = pManager;
    nSavedLocked = nWasLocked;
    bFedCameraMove = false;
    bFedCameraLook = false;
    fMouseLookX = 0.0f;
    fMouseLookY = 0.0f;

    return true;
}

// ------------------------------------------------------------------------------------------------
// Camera input, written onto the camera's own axis fields. The retail free_camera mapping fills
// them from a pad but has no binding for mouse look, which is sampled in the tick.

static void FeedCameraInput(void* pCamera)
{
    if (pCamera == nullptr)
        return;

    auto pComponent = reinterpret_cast<uintptr_t>(pCamera);
    auto bFocused = HasFocus();

    *reinterpret_cast<float*>(pComponent + nCameraSpeed) = fFreecamBaseSpeed * fSpeedSteps[nSpeedStep];

    // ---- movement ----
    auto fForward = (bFocused && KeyDown(nKeyForward) ? 1.0f : 0.0f) - (bFocused && KeyDown(nKeyBack) ? 1.0f : 0.0f);
    auto fStrafe = (bFocused && KeyDown(nKeyStrafeRight) ? 1.0f : 0.0f) - (bFocused && KeyDown(nKeyStrafeLeft) ? 1.0f : 0.0f);
    auto fVertical = (bFocused && KeyDown(nKeyUp) ? 1.0f : 0.0f) - (bFocused && KeyDown(nKeyDown) ? 1.0f : 0.0f);

    if (fForward != 0.0f || fStrafe != 0.0f || fVertical != 0.0f)
    {
        // Unit axes. Speed is on the field written above.
        *reinterpret_cast<float*>(pComponent + nCameraMoveForward) = fForward;
        *reinterpret_cast<float*>(pComponent + nCameraMoveStrafe) = fStrafe;
        *reinterpret_cast<float*>(pComponent + nCameraMoveVertical) = fVertical;

        bFedCameraMove = true;
    }
    else if (bFedCameraMove)
    {
        // The engine's handler only touches these when an action fires, so a released key coasts.
        *reinterpret_cast<float*>(pComponent + nCameraMoveForward) = 0.0f;
        *reinterpret_cast<float*>(pComponent + nCameraMoveStrafe) = 0.0f;
        *reinterpret_cast<float*>(pComponent + nCameraMoveVertical) = 0.0f;

        bFedCameraMove = false;
    }

    // ---- look ----
    // The mouse arrives in the same units the keys produce, and is consumed rather than held.
    auto fYawKeys = (bFocused && KeyDown(nKeyLookRight) ? 1.0f : 0.0f) - (bFocused && KeyDown(nKeyLookLeft) ? 1.0f : 0.0f);
    auto fPitchKeys = (bFocused && KeyDown(nKeyLookUp) ? 1.0f : 0.0f) - (bFocused && KeyDown(nKeyLookDown) ? 1.0f : 0.0f);

    auto fYaw = fYawKeys * fLookRate + fMouseLookX;

    // The pitch field integrates with a negated scale, so up is negative.
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

// ------------------------------------------------------------------------------------------------
// Hooks.

static SafetyHookInline SaveDiamondPropertyHook{};
static SafetyHookInline GameSignalHook{};
static SafetyHookInline FreeCameraUpdateHook{};

// The engine's int32 property writer, shared by every int32 property in the game, hence filtering
// the descriptor and not the object. ESI takes the value, not the field, so the substitution goes
// around the call. Serialisation is single threaded, so nothing can observe it.
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

// The dispatcher all three signals are acted on in, so refusing them here refuses them everywhere.
// True is how the original ends its own refusal paths; false would let the signal through.
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

// CCameraFreeComponent::Update, which CCameraGhostComponent::Update also calls, so one hook drives
// both cameras. ECX is the component's second base, four bytes into the object.
static void __fastcall FreeCameraUpdate(void* pInterface, void* pEdx, float fDelta, uint32_t nFlags)
{
    auto pCamera = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pInterface) - 4);

    if (eCameraMode == CameraMode::Freecam && pCamera == pDebugCamera)
        FeedCameraInput(pCamera);

    FreeCameraUpdateHook.fastcall(pInterface, pEdx, fDelta, nFlags);
}

// ------------------------------------------------------------------------------------------------

static void ReadSettings()
{
    bInvincibility = JackalFixSettings.GetInt(PREF_DEBUGINVINCIBILITY) != 0;
    bInfiniteAmmo = JackalFixSettings.GetInt(PREF_DEBUGINFINITEAMMO) != 0;
    bUnlockAllWeapons = JackalFixSettings.GetInt(PREF_DEBUGUNLOCKALLWEAPONS) != 0;

    nDiamondTarget = JackalFixSettings.GetInt(PREF_DEBUGDIAMONDS);

    bNoclipEnabled = JackalFixSettings.GetInt(PREF_DEBUGNOCLIP) != 0;
    bFreecamEnabled = JackalFixSettings.GetInt(PREF_DEBUGFREECAM) != 0;

    nNoclipKey = ParseKeyName(JackalFixSettings.GetString(PREF_DEBUGNOCLIPKEY));
    nFreecamKey = ParseKeyName(JackalFixSettings.GetString(PREF_DEBUGFREECAMKEY));

    // One key cannot mean two modes. Noclip is first in the ini, so it keeps the key.
    if (nFreecamKey != 0 && nFreecamKey == nNoclipKey)
        nFreecamKey = 0;
}

// Once a frame from CPawnInputListener::Update, on the branch taken when gameplay input is enabled.
// A camera update has no equivalent of that branch, which is what stops W flying the camera while
// the player walks a menu cursor with it.
static void Tick(uintptr_t nListener, uintptr_t nPawn)
{
    // The input pass carries no delta. A hitch or a load comes back as zero, not a huge step.
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);

    auto fFrameDelta = 0.0f;
    if (nNoclipLastCounter != 0 && frequency.QuadPart != 0)
        fFrameDelta = static_cast<float>(static_cast<double>(counter.QuadPart - nNoclipLastCounter) / static_cast<double>(frequency.QuadPart));
    nNoclipLastCounter = counter.QuadPart;

    if (fFrameDelta < 0.0f || fFrameDelta > 0.25f)
        fFrameDelta = 0.0f;

    ApplyProfileFlags();
    ApplyDiamonds();

    // Mouse look for the free camera. By now the frame's input sits in the listener's accumulators
    // in the units the camera wants. Not off the mouse driver: its move handler keeps the converted
    // deltas live in XMM2 and XMM3, and mid hooks do not preserve XMM. Skipped on a pad, where the
    // engine's mapping already feeds these.
    if (eCameraMode == CameraMode::Freecam && nListener != 0 && !IsPadActiveDevice())
    {
        fMouseLookX = *reinterpret_cast<float*>(nListener + nListenerLookX) * fMouseLookGain;
        fMouseLookY = *reinterpret_cast<float*>(nListener + nListenerLookY) * fMouseLookGain;
    }

    if (eCameraMode == CameraMode::Noclip)
    {
        // The entity going away is a level load. Drop the reference rather than follow it.
        if (FocusEntity(pNoclipEntityRef) == nullptr)
        {
            LeaveNoclip();
            eCameraMode = CameraMode::None;
        }
        else
        {
            ApplyNoclip(nListener, nPawn, fFrameDelta);
        }
    }

    // A level load or a cutscene taking the camera ends the mode where it stands.
    if (eCameraMode == CameraMode::Freecam && !DebugCameraStillOurs())
        ForgetCameraMode();

    // Switching a mode off in the ini while it is engaged has to put the player back.
    if (eCameraMode == CameraMode::Noclip && !bNoclipEnabled)
    {
        LeaveNoclip();
        eCameraMode = CameraMode::None;
    }
    else if (eCameraMode == CameraMode::Freecam && !bFreecamEnabled)
    {
        LeaveCameraMode();
    }

    if (!HasFocus())
        return;

    static bool bNoclipLatch = false;
    static bool bFreecamLatch = false;
    static bool bSpeedLatch = false;
    auto bNoclipEdge = KeyPressed(nNoclipKey, bNoclipLatch);
    auto bFreecamEdge = KeyPressed(nFreecamKey, bFreecamLatch);

    // Only while a mode is up. Outside one, both of these are the sprint binding.
    static bool bPadSpeedLatch = false;

    XInputGamepad Pad{};
    auto bPadSpeedEdge = ReadPad(Pad) && PadPressed(Pad.nButtons, nPadSpeedCycle, bPadSpeedLatch);

    if ((KeyPressed(nKeySpeedCycle, bSpeedLatch) || bPadSpeedEdge) && eCameraMode != CameraMode::None)
    {
        nSpeedStep = (nSpeedStep + 1) % SpeedStepCount();
    }

    if (bNoclipEnabled && bNoclipEdge)
    {
        if (eCameraMode == CameraMode::Noclip)
        {
            LeaveNoclip();
            eCameraMode = CameraMode::None;
        }
        else
        {
            LeaveCameraMode();
            if (EnterNoclip())
            {
                eCameraMode = CameraMode::Noclip;

                // A step chosen in freecam can sit past noclip's shorter cycle.
                if (nSpeedStep >= nNoclipSpeedSteps)
                    nSpeedStep = nNoclipSpeedSteps - 1;
            }
        }
    }
    else if (bFreecamEnabled && bFreecamEdge)
    {
        if (eCameraMode == CameraMode::Freecam)
        {
            LeaveCameraMode();
        }
        else
        {
            LeaveNoclip();
            eCameraMode = CameraMode::None;
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
            ReadSettings();

            // CWeaponBazaar::IsWeaponUnlocked, at the AllWeaponsUnlock test. Read only for the
            // GameProfile global.
            //
            //   10737C06  MOV  EAX, [<GameProfile>]
            //   10737C0B  CMP  dword [EAX+0xA0], 0
            auto profilePattern = dunia_pattern("A1 ? ? ? ? 83 B8 A0 00 00 00 00");
            if (!profilePattern.empty())
                ppGameProfile = *reinterpret_cast<void***>(profilePattern.get_first(1));

            // The two act gates, immediately above that test in the same function.
            //
            //   10737BD8  CMP  byte [ESI+0x58], 1     ; northern map
            //   10737BDC  JNZ  +9
            //   10737BDE  CALL <A1GM00 reached>
            //   10737BE3  TEST AL, AL
            //   10737BE5  JZ   fail
            //   10737BE7  CMP  byte [ESI+0x58], 2     ; southern map
            auto actGatePattern = dunia_pattern("80 7E 58 01 75 09 E8 ? ? ? ? 84 C0 74 11 80 7E 58 02");
            if (!actGatePattern.empty())
            {
                static raw_mem fnFirstActGate(actGatePattern.get_first(3), { 0xFF });
                static raw_mem fnSecondActGate(actGatePattern.get_first(18), { 0xFF });

                // Read from the settings, not bUnlockAllWeapons: both sit on the file-watch event,
                // which runs in registration order, so the mirror is a change behind here.
                static auto ActGateCB = []()
                {
                    if (JackalFixSettings.GetInt(PREF_DEBUGUNLOCKALLWEAPONS) != 0)
                    {
                        fnFirstActGate.Write();
                        fnSecondActGate.Write();
                    }
                    else
                    {
                        fnFirstActGate.Restore();
                        fnSecondActGate.Restore();
                    }
                };

                ActGateCB();

                JackalFix::onIniFileChange() += []()
                {
                    ActGateCB();
                };

                JackalFix::onShutdownEvent() += []()
                {
                    fnFirstActGate.Restore();
                    fnSecondActGate.Restore();
                };
            }

            // CEconomyComponent::AddDiamonds. Only read for which component belongs to the player.
            auto addDiamondsPattern = dunia_pattern("51 A1 ? ? ? ? A8 01 56 57 8B F9 75 12 83 C8 01 A3 ? ? ? ? C7 05 ? ? ? ? A8 64 B0 93");
            if (!addDiamondsPattern.empty())
            {
                static auto AddDiamondsHook = safetyhook::create_mid(addDiamondsPattern.get_first(), [](SafetyHookContext& regs)
                {
                    NoteEconomy(reinterpret_cast<void*>(regs.ecx));
                });
            }

            // CEconomyComponent::RemoveDiamonds, before its clamp and store. Spending comes out of
            // the grant first.
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

            // The bazaar page snapshotting the wallet as it opens. One more sighting, for a save
            // that never touches the two functions above.
            auto bazaarSnapshotPattern = dunia_pattern("8B 40 10 89 86 70 01 00 00 89 86 74 01 00 00 89 86 78 01 00 00");
            if (!bazaarSnapshotPattern.empty())
            {
                static auto BazaarSnapshotHook = safetyhook::create_mid(bazaarSnapshotPattern.get_first(), [](SafetyHookContext& regs)
                {
                    NoteEconomy(reinterpret_cast<void*>(regs.eax));
                });
            }

            // CConstIntProperty::Serialise, shared by every int32 property in the game.
            auto savePropertyPattern = dunia_pattern("56 8B C1 8B 70 0C 8B 4C 24 0C 8B 11 57 8B 7C 24 0C 8B 34 3E 83 C0 04 56 50 8B 82 A0 00 00 00 FF D0");
            if (!savePropertyPattern.empty())
                SaveDiamondPropertyHook = safetyhook::create_inline(savePropertyPattern.get_first(), SaveDiamondProperty);

            // CEconomyComponent's constructor: where the wallet is discovered and an outstanding
            // grant is dropped. The int32 property reader would be the obvious place, but
            // renderconfig.ixx inline hooks its entry, and a pattern anchored on bytes another module
            // overwrites stops matching once that module installs.
            //
            //   1066B900  PUSH ESI                  <- pattern starts here
            //   1066B901  MOV  ESI, ECX
            //   1066B903  CALL <base component ctor>
            //   1066B908  XOR  EAX, EAX
            //   1066B90A  MOV  [ESI+0x10], EAX      ; DiamondCount = 0
            //   1066B90D  MOV  [ESI], <vtable>
            //   1066B913  MOV  [ESI+0x04], <vtable>
            //   1066B91A  MOV  [ESI+0x14], EAX      <- hook. Both vtables written, ESI is the object
            //
            // A new component is a new session, and arrives before the save's real count is read
            // into it, since this module's clock does not tick until a pawn exists. So the first
            // top-up is on the far side of the load.
            auto economyCtorPattern = dunia_pattern("56 8B F1 E8 ? ? ? ? 33 C0 89 46 10 C7 06 ? ? ? ? C7 46 04 ? ? ? ? 89 46 14 89 46 18 89 46 1C 89 46 20 89 46 24 89 46 28 8B C6 5E C3");
            if (!economyCtorPattern.empty())
            {
                static auto EconomyCtorHook = safetyhook::create_mid(economyCtorPattern.get_first(nEconomyCtorVTableSet), [](SafetyHookContext& regs)
                {
                    nGranted = 0;
                    NoteEconomy(reinterpret_cast<void*>(regs.esi));
                });
            }

            // CPlayer::GetLocalPlayer and the two camera manager entry points.
            auto localPlayerPattern = dunia_pattern("A1 ? ? ? ? 83 78 08 00 75 03 33 C0 C3 8B 40 04 8B 00 C3");
            auto setCameraPattern = dunia_pattern("51 53 55 56 8B F1 80 7E 14 00 57 0F 85");
            auto getCameraPattern = dunia_pattern("8B 41 10 85 C0 7C 20 3B 41 1C 73 1B 51 8B 49 18 8D 04 40");

            if (!localPlayerPattern.empty() && !setCameraPattern.empty() && !getCameraPattern.empty())
            {
                GetLocalPlayer = reinterpret_cast<GetLocalPlayer_t>(localPlayerPattern.get_first());
                SetActiveCameraByName = reinterpret_cast<SetActiveCameraByName_t>(setCameraPattern.get_first());
                GetActiveCamera = reinterpret_cast<GetActiveCamera_t>(getCameraPattern.get_first());
            }

            // The manager's resolve-and-set-focus sequence, in the miss path of
            // SetActiveCameraByName. Read, not hooked: the ref world global and the resolver.
            //
            //   1057CCF7  MOV  EDX, [EBP+0x0C]        ; focus id high
            //   1057CCFA  MOV  EAX, [EBP+0x08]        ; focus id low
            //   1057CD04  MOV  ECX, [<ref world>]     <- the global, at +0x0F
            //   1057CD0A  CALL <resolve id to ref>    <- the resolver, at +0x13
            //   1057CD12  CMP  dword [EDI+0x0C], 0    ; entity still alive?
            //   1057CD1A  ADD  dword [EDI+0x08], 1    ; a reference for the call
            //   1057CD22  CALL [EDX+0x78]             ; the camera's SetFocus
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

            // The physics tag, its lazy registrar and the fetch, all three in a row in the ghost
            // camera's activate handler.
            //
            //   10692B3B  CMP  dword [<tag registered>], 0
            //   10692B42  JNZ  10692B4B
            //   10692B44  XOR  ECX, ECX
            //   10692B46  CALL <register the tag>
            //   10692B4B  PUSH <the tag>
            //   10692B50  MOV  ECX, ESI                  ; the player entity
            //   10692B52  CALL <get component by tag>
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

            // CEntity::SetPosition. Only patternable because SetEuler sits behind it.
            auto setPositionPattern = dunia_pattern("6A 00 8D 44 24 08 50 E8 ? ? ? ? C2 0C 00 CC 6A 00 8D 44 24 08 50 E8 ? ? ? ? C2 0C 00");
            if (!setPositionPattern.empty())
            {
                SetEntityPosition = reinterpret_cast<SetEntityPosition_t>(setPositionPattern.get_first());
                SetEntityEuler = reinterpret_cast<SetEntityEuler_t>(setPositionPattern.get_first(0x10));
            }

            // The render camera accessor, where noclip reads the pitch it starts from.
            auto renderCameraPattern = dunia_pattern("83 C1 14 6A 01 51 B9 ? ? ? ? E8 ? ? ? ? C3");
            if (!renderCameraPattern.empty())
                GetRenderCamera = reinterpret_cast<GetRenderCamera_t>(renderCameraPattern.get_first());

            // The signal dispatcher. Opens with a five byte MOV EAX,[imm32], nothing to relocate.
            auto gameSignalPattern = dunia_pattern("A1 ? ? ? ? 83 EC 50 A8 01 53 55 56 57 8B F9");
            if (!gameSignalPattern.empty())
                GameSignalHook = safetyhook::create_inline(gameSignalPattern.get_first(), GameSignal);

            // CCameraFreeComponent::Update.
            auto freeCameraPattern = dunia_pattern("83 EC 28 53 55 56 57 8B 3D ? ? ? ? 8B F1 8D 44 24 14 8D 4E FC 50");
            if (!freeCameraPattern.empty())
                FreeCameraUpdateHook = safetyhook::create_inline(freeCameraPattern.get_first(), FreeCameraUpdate);

            // CPawnInputListener::Update, the module's clock.
            //
            //   10144AA0  XOR  EAX, EAX             ; the entry lookback.ixx hooks
            //   10144AA2  CMP  byte [ESP+0x8], AL   ; input disabled?
            //   10144AA6  PUSH ESI                  <- pattern starts here
            //   10144AA7  MOV  ESI, ECX             ; the listener
            //   10144AA9  JZ   10144AB5             ; enabled, carry on
            //   10144AAB  MOV  byte [ESI+0x4], AL   ; disabled: clear and return
            //   10144AB2  RET  8
            //   10144AB5  MOV  ECX, [ESI+0x20]      ; the pawn
            //   10144ABA  JZ   10144B06             ; no pawn, nothing to do
            //   10144ABC  CALL <get aim state>      <- hook. ESI is the listener
            //
            // Both addresses are chosen to survive lookback.ixx, which inline hooks the entry: the
            // pattern starts past the six bytes its jump overwrites, and the hook goes past the end
            // of lookback's own pattern, so neither can stop the other matching in either install
            // order. Landing on a call also makes float work safe here, XMM being caller-saved.
            auto inputPassPattern = dunia_pattern("56 8B F1 74 0A 88 46 04 88 46 05 5E C2 08 00 8B 4E 20 3B C8 74 4A E8 ? ? ? ? F6 40 04 40 74 11");
            if (!inputPassPattern.empty())
            {
                static auto InputPassHook = safetyhook::create_mid(inputPassPattern.get_first(nInputPassLivePawn), [](SafetyHookContext& regs)
                {
                    // ESI is the listener, ECX the pawn the relocated instruction just fetched.
                    Tick(static_cast<uintptr_t>(regs.esi), static_cast<uintptr_t>(regs.ecx));
                });
            }

            JackalFix::onIniFileChange() += []()
            {
                ReadSettings();
            };

            // State only. Shutdown runs from DllMain's detach path under the loader lock, so a
            // camera switch there buys nothing. The act-gate restore above is only a memcpy.
            JackalFix::onShutdownEvent() += []()
            {
                eCameraMode = CameraMode::None;
                pDebugCamera = nullptr;
                pDebugCameraManager = nullptr;
                nSavedLocked = 0;
                bFedCameraMove = false;
                bFedCameraLook = false;
            };
        };
    }
} Debug;
