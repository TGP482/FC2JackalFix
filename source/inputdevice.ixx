module;

#include <common.hxx>
#include <cstdint>

export module inputdevice;

import common;
import dunia;

// Which device the player currently has in their hands.
//
// Dunia's action map is device agnostic by the time an action reaches gameplay code: a bound
// action carries no record of whether a key, a mouse button or a pad button produced it. That
// leaves the raw driver layer as the only place the distinction still exists, so this watches the
// three input drivers directly. Each one is a CInputDriver subclass with its per frame poll at
// vtable+0x0C, and each is hooked at the point where it has real device state in hand:
//
//     CInputDriverGamepad::Poll   0x102C99E0   XInputGetState into this+0x14
//     CInputDriverKeyboard::OnKey 0x102CD950   per key, from the DirectInput event buffer
//     CInputDriverMouse::OnButton 0x102C88C0   per button, from the DirectInput event buffer
//     CInputDriverMouse::OnMove   pattern      accumulated x/y for the frame
//
// An earlier version of this watched only the look axes, on the assumption that nobody plays for
// long without moving the view. That assumption is wrong in the one place it matters: the flag
// only flipped when the *camera* moved, so firing, throwing, reloading, driving and taking damage
// all left it stale. A pad would keep rumbling through a mouse and keyboard firefight until the
// player happened to swing the view, and a pad player who had touched the mouse got nothing back
// until they moved the right stick. Every input now votes, which is what the flag always claimed
// to mean.
static bool bPadIsActiveDevice = false;

// Standard XInput deadzones. The engine applies its own further down, but these are what keeps a
// worn stick resting off centre from claiming the pad is in use - the poll below runs every frame
// whether or not the state changed, so drift would otherwise pin the flag on permanently.
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

// Distance from the entry of the gamepad poll to the packet number comparison. Hooking here
// rather than at the entry puts it past the XInputGetState failure branch, so an unplugged pad
// never reports activity, and leaves a fresh XINPUT_STATE in the object.
static constexpr ptrdiff_t nPadStateReady = 0x1F;

// DirectInput reports a byte per key and per button, high bit set while held.
static constexpr uint8_t nPressedBit = 0x80;

// Offset of the button bytes inside the mouse driver's frame state block, after the x, y and
// wheel deltas.
static constexpr ptrdiff_t nMouseButtons = 0x0C;
static constexpr int32_t nMouseButtonCount = 8;
static constexpr int32_t nKeyboardKeyCount = 256;

export bool IsPadActiveDevice()
{
    return bPadIsActiveDevice;
}

// Squared to keep the comparison in integers. Two full scale axes come to 2,147,352,578, which
// clears a signed 32 bit maximum by less than a thousand, so this is deliberately 64 bit.
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
            // CInputDriverGamepad::Poll, immediately after a successful XInputGetState.
            //
            //   102C99E0  PUSH  ESI
            //   102C99E1  MOV   ESI,ECX              ; the driver
            //   102C99EF  MOV   [ESI+0x24],EAX       ; previous packet number
            //   102C99F2  CALL  XInputGetState
            //   102C99F7  TEST  EAX,EAX
            //   102C99F9  JNZ   out                  ; not connected, nothing to read
            //   102C99FF  MOV   EDX,[ESI+0x24]       <- hook
            //   102C9A02  CMP   EDX,[EDI]            ; the engine skips ahead when unchanged
            //
            // Reading the state rather than the engine's packet number comparison is what makes
            // a held button count: XInput only bumps the packet on a change, so a finger resting
            // on a trigger would otherwise look identical to an idle pad.
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
                        bPadIsActiveDevice = true;
                    }
                });
            }

            // CInputDriverKeyboard::OnKey(keyStates, sink, keyIndex), one call per key that
            // changed this frame. Only a press counts, which also excludes the 256 key sweep the
            // poll runs when DirectInput reports the device lost - that sweep hands over a zeroed
            // state array, so every key reads as released.
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
                        bPadIsActiveDevice = false;
                });
            }

            // CInputDriverMouse::OnButton(sink, frameState, buttonIndex, controlId). This is the
            // one that catches firing a weapon on mouse and keyboard, which is the case the look
            // axis heuristic used to miss entirely. Same released-state reasoning as the keyboard
            // covers the device lost sweep here.
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
                        bPadIsActiveDevice = false;
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
                        bPadIsActiveDevice = false;
                });
            }
        };
    }
} InputDevice;
