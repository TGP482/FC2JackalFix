module;

#include <common.hxx>
#include <atomic>
#include <cstdint>

export module inputdevice;

import common;
import dunia;

// Which device is in the player's hands. The action map has lost that by the time it reaches
// gameplay, so watch the raw drivers.
static bool bPadIsActiveDevice = false;

// Standard XInput deadzones; the poll runs every frame, so drift would pin the flag on.
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

// Gamepad poll entry to the packet compare: past the XInputGetState failure branch.
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

// State from the driver's last successful poll. Stale, not zeroed, once unplugged.
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

// Buttons the pad driver keeps this frame. A/B become UI Enter/Escape before a nav bar prompt can
// see them, so clear the bits in the driver's own XINPUT_STATE ahead of the poll. Atomic: two threads.
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

// Fires on the flip only, so page-rebuild code can notice the pad being put down mid screen.
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
            // CInputDriverGamepad::Poll after a successful XInputGetState; ESI is the driver. Reads
            // state, not the packet compare at 102C9A02: XInput bumps the packet only on a change.
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

                    // From the raw read, so the mask never hides a press from the device vote.
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

            // CInputDriverKeyboard::OnKey(keyStates, sink, keyIndex). Press only, which also excludes
            // the device-lost 256 key sweep (zeroed states). GOG reads +8 as a byte, Steam as a word.
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

            // CInputDriverMouse::OnButton(sink, frameState, buttonIndex, controlId). Press only, as above.
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

            // CInputDriverMouse::OnMove, given x/y accumulated over the frame.
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
