module;

#include <common.hxx>

export module menucursor;

import common;
import dunia;

// Mouse cursor movement goes through the gamepad stick path and inherits its per-frame rounding.
// Keep the remainder so small mouse movements are not lost.

static float fResidualX = 0.0f;
static float fResidualY = 0.0f;

static float CarryRemainder(float delta, float& residual)
{
    auto total = delta + residual;
    residual = total - std::trunc(total);
    return total;
}

class MenuCursor
{
public:
    MenuCursor()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            auto pattern = dunia_pattern("F3 0F 10 86 88 00 00 00 0F 28 E0 F3 0F 59 E2 F3 0F 59 E3 F3 0F 2C CC");
            if (pattern.empty())
                return;

            static auto CursorDeltaXHook = safetyhook::create_mid(pattern.get_first(19), [](SafetyHookContext& regs)
            {
                regs.xmm4.f32[0] = CarryRemainder(regs.xmm4.f32[0], fResidualX);
            });

            pattern = dunia_pattern("F3 0F 59 C1 F3 0F 59 C3 C1 E8 10 F3 0F 2C D0 66 2B C2");
            if (pattern.empty())
                return;

            static auto CursorDeltaYHook = safetyhook::create_mid(pattern.get_first(11), [](SafetyHookContext& regs)
            {
                regs.xmm0.f32[0] = CarryRemainder(regs.xmm0.f32[0], fResidualY);
            });
        };
    }
} MenuCursor;
