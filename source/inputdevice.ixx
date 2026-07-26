module;

#include <common.hxx>

export module inputdevice;

import common;
import dunia;

// Dunia's action map is device agnostic by the time an action reaches gameplay code: a bound
// action carries no record of whether a key, a mouse button or a pad button produced it. The look
// axes are the one place where the engine still distinguishes the two, so the last device to move
// the camera is used as a stand-in for "what the player is holding right now". It is a heuristic,
// but a reliable one in practice - nobody plays for long without moving the view.
static bool bPadIsLookDevice = false;

// Look axes in the engine's axis list. Anything outside this range is movement or vehicle input.
static constexpr uintptr_t nFirstLookAxis = 6;
static constexpr uintptr_t nLastLookAxis = 7;

export bool IsPadActiveDevice()
{
    return bPadIsLookDevice;
}

class InputDevice
{
public:
    InputDevice()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Marks controller as the active look device when right stick input exceeds
            // the engine dead zone.
            auto padAxisPattern = dunia_pattern("0F 28 C8 0F 54 CA F3 0F 10 51 08 56 0F 2F D1 0F 57 C9 57");
            if (!padAxisPattern.empty())
            {
                static auto PadLookDeviceHook = safetyhook::create_mid(padAxisPattern.get_first(12), [](SafetyHookContext& regs)
                {
                    if (regs.edx < nFirstLookAxis || regs.edx > nLastLookAxis)
                        return;

                    if (regs.xmm1.f32[0] > regs.xmm2.f32[0])
                        bPadIsLookDevice = true;
                });
            }

            // Marks mouse as the active look device when mouse movement is detected.
            auto mouseAxisPattern = dunia_pattern("8B 44 24 08 F3 0F 2A 10 F3 0F 2A 58 04 83 EC 14 53 55 8B E9 0F 2E 55 18");
            if (!mouseAxisPattern.empty())
            {
                static auto MouseLookDeviceHook = safetyhook::create_mid(mouseAxisPattern.get_first(), [](SafetyHookContext& regs)
                {
                    auto pAccumulator = *(int**)(regs.esp + 8);
                    if (pAccumulator == nullptr)
                        return;

                    if (pAccumulator[0] != 0 || pAccumulator[1] != 0)
                        bPadIsLookDevice = false;
                });
            }
        };
    }
} InputDevice;
