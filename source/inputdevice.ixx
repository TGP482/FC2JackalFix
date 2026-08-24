module;

#include <common.hxx>
#include <atomic>
#include <cstdint>

export module inputdevice;

import common;
import dunia;

// Which device the player currently has in their hands.
//
// The action map has forgotten which device produced an action by the time it reaches gameplay
// code, so this watches the raw drivers, hooked where each has real device state in hand:
//
//     CInputDriverGamepad::Poll   0x102C99E0   XInputGetState into this+0x14
//     CInputDriverKeyboard::OnKey 0x102CD950   per key, from the DirectInput event buffer
//     CInputDriverMouse::OnButton 0x102C88C0   per button, from the DirectInput event buffer
//     CInputDriverMouse::OnMove   pattern      accumulated x/y for the frame
//
// Every input votes. Watching only the look axes leaves the flag stale through a whole firefight.
static bool bPadIsActiveDevice = false;

// Standard XInput deadzones. The poll runs every frame regardless of state changes, so stick
// drift would otherwise pin the flag on permanently.
static constexpr int32_t nLeftThumbDeadzone = 7849;
static constexpr int32_t nRightThumbDeadzone = 8689;
static constexpr int32_t nTriggerThreshold = 30;

// XINPUT_STATE lives at this+0x14 on the gamepad driver: dwPacketNumber, then XINPUT_GAMEPAD.
static constexpr ptrdiff_t nPadButtons = 0x18;
static constexpr ptrdiff_t nPadLeftTrigger = 0x1A;
static constexpr ptrdiff_t nPadRightTrigger = 0x1B;
static constexpr ptrdiff_t nPadThumbLX = 0x1C;
static constexpr ptrdiff_t nPadThumbLY = 0x1E;
static constexpr ptrdiff_t nPadThumbRX = 0x20;
static constexpr ptrdiff_t nPadThumbRY = 0x22;

// Entry of the gamepad poll to the packet number comparison. Past the XInputGetState failure
// branch, so an unplugged pad never reports activity, and XINPUT_STATE is fresh.
static constexpr ptrdiff_t nPadStateReady = 0x1F;

// DirectInput reports a byte per key and per button, high bit set while held.
static constexpr uint8_t nPressedBit = 0x80;

// Button bytes in the mouse driver's frame state block, after the x, y and wheel deltas.
static constexpr ptrdiff_t nMouseButtons = 0x0C;
static constexpr int32_t nMouseButtonCount = 8;
static constexpr int32_t nKeyboardKeyCount = 256;

export bool IsPadActiveDevice()
{
    return bPadIsActiveDevice;
}

// The state the driver's last successful poll had in hand, so nothing else has to open its own
// XInput. Stale, not zeroed, once the pad is unplugged: the hook is past the failure branch.
export struct PadState
{
    uint16_t nButtons;
    int16_t sThumbLX;
    int16_t sThumbLY;
};

static PadState LastPad{};

export const PadState& GetPadState()
{
    return LastPad;
}

// Buttons the pad driver is allowed to keep this frame. The menus hand a few of the pad's face
// buttons to their own nav bar prompts, and the action map turns A and B into the UI's Enter and
// Escape long before a prompt could be reached, so the bits are cleared in the driver's own
// XINPUT_STATE, ahead of the poll that reads it into actions.
//
// Written from the game thread and read on whichever thread polls, so it is atomic rather than
// guarded: one stale frame either way is a button that acts once late.
static std::atomic<uint16_t> nPadButtonMask{ 0xFFFF };

export void SetPadButtonMask(uint16_t nMask)
{
    nPadButtonMask = nMask;
}

// Deadzoned and normalised, so a worn stick resting off centre is not input.
export float PadThumbAxis(int16_t sValue)
{
    auto fValue = static_cast<float>(sValue);
    if (std::abs(fValue) < nLeftThumbDeadzone)
        return 0.0f;

    constexpr auto fRange = 32767.0f - nLeftThumbDeadzone;

    return std::clamp((fValue - std::copysign(static_cast<float>(nLeftThumbDeadzone), fValue)) / fRange, -1.0f, 1.0f);
}

// Fires on the flip rather than on every input, so anything that only rebuilds when a page changes
// can notice the player put the pad down mid screen.
export JackalFix::Event<>& onInputDeviceChange()
{
    static JackalFix::Event<> InputDeviceChange;
    return InputDeviceChange;
}

static void SetPadActiveDevice(bool bPad)
{
    if (bPadIsActiveDevice == bPad)
        return;

    bPadIsActiveDevice = bPad;
    onInputDeviceChange().executeAll();
}

// 64 bit deliberately: two full scale axes square to 2,147,352,578, just over INT32_MAX.
static bool ThumbOutsideDeadzone(int16_t sX, int16_t sY, int32_t nDeadzone)
{
    auto nX = static_cast<int64_t>(sX);
    auto nY = static_cast<int64_t>(sY);
    auto nLimit = static_cast<int64_t>(nDeadzone);

    return nX * nX + nY * nY > nLimit * nLimit;
}

class InputDevice
{
public:
    InputDevice()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // CInputDriverGamepad::Poll, immediately after a successful XInputGetState. ESI is
            // the driver. Reads the state rather than the engine's packet number comparison at
            // 102C9A02, so a held button counts: XInput only bumps the packet on a change.
            if (auto* pPadPoll = dunia_find("56 8B F1 8B 4E 0C 8B 46 14 57 8D 7E 14 57 51 89 46 24 E8 ? ? ? ? 85 C0 0F 85", nPadStateReady))
            {
                static auto PadPollHook = safetyhook::create_mid(pPadPoll, [](SafetyHookContext& regs)
                {
                    auto pPad = regs.esi;

                    auto nButtons = *reinterpret_cast<uint16_t*>(pPad + nPadButtons);
                    auto nLeftTrigger = *reinterpret_cast<uint8_t*>(pPad + nPadLeftTrigger);
                    auto nRightTrigger = *reinterpret_cast<uint8_t*>(pPad + nPadRightTrigger);
                    auto sThumbLX = *reinterpret_cast<int16_t*>(pPad + nPadThumbLX);
                    auto sThumbLY = *reinterpret_cast<int16_t*>(pPad + nPadThumbLY);
                    auto sThumbRX = *reinterpret_cast<int16_t*>(pPad + nPadThumbRX);
                    auto sThumbRY = *reinterpret_cast<int16_t*>(pPad + nPadThumbRY);

                    LastPad = { nButtons, sThumbLX, sThumbLY };

                    // Taken from the raw read, so the mask never hides a press from the device
                    // vote or from whoever asked for the mask in the first place.
                    auto nMask = nPadButtonMask.load();
                    if (nMask != 0xFFFF)
                        *reinterpret_cast<uint16_t*>(pPad + nPadButtons) = static_cast<uint16_t>(nButtons & nMask);

                    if (nButtons != 0
                        || nLeftTrigger > nTriggerThreshold
                        || nRightTrigger > nTriggerThreshold
                        || ThumbOutsideDeadzone(sThumbLX, sThumbLY, nLeftThumbDeadzone)
                        || ThumbOutsideDeadzone(sThumbRX, sThumbRY, nRightThumbDeadzone))
                    {
                        SetPadActiveDevice(true);
                    }
                });
            }

            // CInputDriverKeyboard::OnKey(keyStates, sink, keyIndex), one call per changed key.
            // Only a press counts, which also excludes the 256 key sweep the poll runs on device
            // lost: that sweep passes a zeroed state array, so every key reads as released.
            // GOG reads the flag field at +8 as a byte where Steam reads it as a word, so the
            // patterns part company four instructions in. Everything the hook below reads, the
            // entry stack layout, is identical in both.
            if (auto* pKey = dunia_find(
                "8B 44 24 0C 83 EC 30 8D 14 C5 00 00 00 00 2B D0 56 8D 34 91 0F B7 56 08",
                "8B 44 24 0C 83 EC 30 8D 14 C5 00 00 00 00 2B D0 56 8D 34 91 8A 56 08 F6 C2 01"))
            {
                static auto KeyboardHook = safetyhook::create_mid(pKey, [](SafetyHookContext& regs)
                {
                    auto pKeyStates = *reinterpret_cast<uint8_t**>(regs.esp + 4);
                    auto nKey = *reinterpret_cast<int32_t*>(regs.esp + 0xC);

                    if (pKeyStates == nullptr || nKey < 0 || nKey >= nKeyboardKeyCount)
                        return;

                    if (pKeyStates[nKey] & nPressedBit)
                        SetPadActiveDevice(false);
                });
            }

            // CInputDriverMouse::OnButton(sink, frameState, buttonIndex, controlId). Same
            // released-state reasoning as the keyboard covers the device lost sweep here.
            if (auto* pMouseButton = dunia_find("8B 44 24 08 83 EC 30 53 56 57 8B 7C 24 48 8A 5C 38 0C 8B F1 C0 EB 07"))
            {
                static auto MouseButtonHook = safetyhook::create_mid(pMouseButton, [](SafetyHookContext& regs)
                {
                    auto pFrameState = *reinterpret_cast<uint8_t**>(regs.esp + 8);
                    auto nButton = *reinterpret_cast<int32_t*>(regs.esp + 0xC);

                    if (pFrameState == nullptr || nButton < 0 || nButton >= nMouseButtonCount)
                        return;

                    if (pFrameState[nMouseButtons + nButton] & nPressedBit)
                        SetPadActiveDevice(false);
                });
            }

            // CInputDriverMouse::OnMove, given the x/y accumulated over the frame.
            if (auto* pMouseMove = dunia_find("8B 44 24 08 F3 0F 2A 10 F3 0F 2A 58 04 83 EC 14 53 55 8B E9 0F 2E 55 18"))
            {
                static auto MouseMoveHook = safetyhook::create_mid(pMouseMove, [](SafetyHookContext& regs)
                {
                    auto pAccumulator = *reinterpret_cast<int32_t**>(regs.esp + 8);
                    if (pAccumulator == nullptr)
                        return;

                    if (pAccumulator[0] != 0 || pAccumulator[1] != 0)
                        SetPadActiveDevice(false);
                });
            }
        };
    }
} InputDevice;
