module;

#include <common.hxx>
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

// Fires on the flip, not on every input. Anything that only rebuilds when a page changes has no
// other way to notice the player put the pad down mid screen.
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
            auto padPollPattern = dunia_pattern("56 8B F1 8B 4E 0C 8B 46 14 57 8D 7E 14 57 51 89 46 24 E8 ? ? ? ? 85 C0 0F 85");
            if (!padPollPattern.empty())
            {
                static auto PadPollHook = safetyhook::create_mid(padPollPattern.get_first(nPadStateReady), [](SafetyHookContext& regs)
                {
                    auto pPad = regs.esi;

                    auto nButtons = *reinterpret_cast<uint16_t*>(pPad + nPadButtons);
                    auto nLeftTrigger = *reinterpret_cast<uint8_t*>(pPad + nPadLeftTrigger);
                    auto nRightTrigger = *reinterpret_cast<uint8_t*>(pPad + nPadRightTrigger);
                    auto sThumbLX = *reinterpret_cast<int16_t*>(pPad + nPadThumbLX);
                    auto sThumbLY = *reinterpret_cast<int16_t*>(pPad + nPadThumbLY);
                    auto sThumbRX = *reinterpret_cast<int16_t*>(pPad + nPadThumbRX);
                    auto sThumbRY = *reinterpret_cast<int16_t*>(pPad + nPadThumbRY);

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
            auto keyPattern = dunia_pattern("8B 44 24 0C 83 EC 30 8D 14 C5 00 00 00 00 2B D0 56 8D 34 91 0F B7 56 08");
            if (!keyPattern.empty())
            {
                static auto KeyboardHook = safetyhook::create_mid(keyPattern.get_first(), [](SafetyHookContext& regs)
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
            auto mouseButtonPattern = dunia_pattern("8B 44 24 08 83 EC 30 53 56 57 8B 7C 24 48 8A 5C 38 0C 8B F1 C0 EB 07");
            if (!mouseButtonPattern.empty())
            {
                static auto MouseButtonHook = safetyhook::create_mid(mouseButtonPattern.get_first(), [](SafetyHookContext& regs)
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
            auto mouseMovePattern = dunia_pattern("8B 44 24 08 F3 0F 2A 10 F3 0F 2A 58 04 83 EC 14 53 55 8B E9 0F 2E 55 18");
            if (!mouseMovePattern.empty())
            {
                static auto MouseMoveHook = safetyhook::create_mid(mouseMovePattern.get_first(), [](SafetyHookContext& regs)
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
