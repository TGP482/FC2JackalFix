module;

#include <common.hxx>
#include <cstring>
#include <mutex>
#include <Xinput.h>
#include <SDL3/SDL.h>

export module gamepads;

import common;
import dunia;
import logging;

// Controllers come through SDL instead of XInput, so PlayStation, Switch and other pads work as the
// Xbox one does. Dunia's two XInput imports read and rumble SDL's pads in XInput's own report.
static constexpr DWORD nPadSlots = 4;

static constexpr const char* szXInputModule = "XINPUT1_3.dll";
static constexpr WORD nXInputGetStateOrdinal = 2;
static constexpr WORD nXInputSetStateOrdinal = 3;

// A pad keeps the lowest free slot from plug in to unplug, as XInput numbers them.
static std::mutex PadsMutex;
static SDL_Gamepad* Pads[nPadSlots]{};
static XINPUT_GAMEPAD LastReport[nPadSlots]{};
static DWORD nPacket[nPadSlots]{};

static constexpr std::pair<SDL_GamepadButton, WORD> PadButtons[]
{
    { SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
    { SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START },
    { SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
    { SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A },
    { SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B },
    { SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X },
    { SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y },
};

static void AddPad(SDL_JoystickID nId)
{
    for (auto* pPad : Pads)
    {
        if (pPad != nullptr && SDL_GetGamepadID(pPad) == nId)
            return;
    }

    auto ppSlot = std::find(std::begin(Pads), std::end(Pads), nullptr);
    if (ppSlot == std::end(Pads))
        return;

    *ppSlot = SDL_OpenGamepad(nId);
    const char* pszName = *ppSlot != nullptr ? SDL_GetGamepadName(*ppSlot) : SDL_GetError();
    LogInfo("controller {} {}: {}", ppSlot - Pads + 1, *ppSlot != nullptr ? "plugged in" : "failed to open", pszName ? pszName : "");
}

static void RemovePad(SDL_JoystickID nId)
{
    for (DWORD i = 0; i < nPadSlots; i++)
    {
        if (Pads[i] == nullptr || SDL_GetGamepadID(Pads[i]) != nId)
            continue;

        SDL_CloseGamepad(Pads[i]);
        Pads[i] = nullptr;
        LogInfo("controller {} unplugged", i + 1);
    }
}

// Once a millisecond at most, however many slots and threads read.
static void UpdatePads()
{
    static ULONGLONG nLastUpdate = 0;

    const auto nNow = GetTickCount64();
    if (nNow == nLastUpdate)
        return;

    nLastUpdate = nNow;
    SDL_PumpEvents();

    SDL_Event event{};
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1)
    {
        if (event.type == SDL_EVENT_GAMEPAD_ADDED)
            AddPad(event.gdevice.which);
        else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
            RemovePad(event.gdevice.which);
    }
}

static BYTE ReadTrigger(SDL_Gamepad* pPad, SDL_GamepadAxis eAxis)
{
    // 0 to 32767 into 0 to 255.
    return static_cast<BYTE>(std::max<int>(SDL_GetGamepadAxis(pPad, eAxis), 0) >> 7);
}

// SDL's sticks have y down; not plain negation, -32768 has no positive.
static SHORT ReadStickUp(SDL_Gamepad* pPad, SDL_GamepadAxis eAxis)
{
    return static_cast<SHORT>(-1 - SDL_GetGamepadAxis(pPad, eAxis));
}

static XINPUT_GAMEPAD ReadPad(SDL_Gamepad* pPad)
{
    XINPUT_GAMEPAD report{};
    for (auto [eButton, nMask] : PadButtons)
    {
        if (SDL_GetGamepadButton(pPad, eButton))
            report.wButtons |= nMask;
    }

    report.bLeftTrigger = ReadTrigger(pPad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    report.bRightTrigger = ReadTrigger(pPad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    report.sThumbLX = SDL_GetGamepadAxis(pPad, SDL_GAMEPAD_AXIS_LEFTX);
    report.sThumbLY = ReadStickUp(pPad, SDL_GAMEPAD_AXIS_LEFTY);
    report.sThumbRX = SDL_GetGamepadAxis(pPad, SDL_GAMEPAD_AXIS_RIGHTX);
    report.sThumbRY = ReadStickUp(pPad, SDL_GAMEPAD_AXIS_RIGHTY);
    return report;
}

static DWORD WINAPI GetPadState(DWORD nSlot, XINPUT_STATE* pState)
{
    if (nSlot >= nPadSlots || pState == nullptr)
        return ERROR_BAD_ARGUMENTS;

    std::scoped_lock lock(PadsMutex);
    UpdatePads();

    if (Pads[nSlot] == nullptr)
        return ERROR_DEVICE_NOT_CONNECTED;

    // XInput bumps the packet on a change only, and Dunia compares it.
    auto report = ReadPad(Pads[nSlot]);
    if (std::memcmp(&report, &LastReport[nSlot], sizeof(report)) != 0)
        nPacket[nSlot]++;

    LastReport[nSlot] = report;
    pState->dwPacketNumber = nPacket[nSlot];
    pState->Gamepad = report;
    return ERROR_SUCCESS;
}

static DWORD WINAPI SetPadState(DWORD nSlot, XINPUT_VIBRATION* pVibration)
{
    if (nSlot >= nPadSlots || pVibration == nullptr)
        return ERROR_BAD_ARGUMENTS;

    std::scoped_lock lock(PadsMutex);
    if (Pads[nSlot] == nullptr)
        return ERROR_DEVICE_NOT_CONNECTED;

    // A duration of 0 holds the motors until the next call, as XInput does.
    SDL_RumbleGamepad(Pads[nSlot], pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed, 0);
    return ERROR_SUCCESS;
}

static bool StartPads()
{
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Dunia reads from its
    // own threads, so SDL's thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");

    if (!SDL_Init(SDL_INIT_GAMEPAD))
    {
        LogWarn("SDL failed to start, controllers stay on XInput: {}", SDL_GetError());
        return false;
    }

    std::scoped_lock lock(PadsMutex);

    int nCount = 0;
    if (auto* pIds = SDL_GetGamepads(&nCount))
    {
        for (int i = 0; i < nCount; i++)
            AddPad(pIds[i]);

        SDL_free(pIds);
    }

    return true;
}

// Ordinal imports only: Dunia takes XInput by number.
static bool HookOrdinalImport(HMODULE hModule, const char* szModule, WORD nOrdinal, void* pDetour)
{
    auto pBase = reinterpret_cast<uint8_t*>(hModule);
    auto pDos = reinterpret_cast<IMAGE_DOS_HEADER*>(pBase);
    auto pNt = reinterpret_cast<IMAGE_NT_HEADERS*>(pBase + pDos->e_lfanew);
    auto& dir = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0)
        return false;

    auto pImport = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(pBase + dir.VirtualAddress);
    for (; pImport->Name != 0; ++pImport)
    {
        if (lstrcmpiA(reinterpret_cast<const char*>(pBase + pImport->Name), szModule) != 0)
            continue;

        auto pThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(pBase + pImport->FirstThunk);
        auto nNamesRva = pImport->OriginalFirstThunk != 0 ? pImport->OriginalFirstThunk : pImport->FirstThunk;
        auto pNames = reinterpret_cast<IMAGE_THUNK_DATA*>(pBase + nNamesRva);

        for (; pNames->u1.AddressOfData != 0; ++pNames, ++pThunk)
        {
            if (!IMAGE_SNAP_BY_ORDINAL(pNames->u1.Ordinal) || IMAGE_ORDINAL(pNames->u1.Ordinal) != nOrdinal)
                continue;

            injector::WriteMemory(&pThunk->u1.Function, reinterpret_cast<uintptr_t>(pDetour), true);
            return true;
        }
    }

    return false;
}

class Gamepads
{
public:
    Gamepads()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            if (!StartPads())
                return;

            HookOrdinalImport(hDunia, szXInputModule, nXInputGetStateOrdinal, &GetPadState);
            HookOrdinalImport(hDunia, szXInputModule, nXInputSetStateOrdinal, &SetPadState);
        };

        JackalFix::onShutdownEvent() += []()
        {
            SDL_Quit();
        };
    }
} Gamepads;
