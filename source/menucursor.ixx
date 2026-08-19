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
            auto* pCursorDeltaX = dunia_find("F3 0F 10 86 88 00 00 00 0F 28 E0 F3 0F 59 E2 F3 0F 59 E3 F3 0F 2C CC", 19);
            if (!pCursorDeltaX)
                return;

            static auto CursorDeltaXHook = safetyhook::create_mid(pCursorDeltaX, [](SafetyHookContext& regs)
            {
                regs.xmm4.f32[0] = CarryRemainder(regs.xmm4.f32[0], fResidualX);
            });

            auto* pCursorDeltaY = dunia_find("F3 0F 59 C1 F3 0F 59 C3 C1 E8 10 F3 0F 2C D0 66 2B C2", 11);
            if (!pCursorDeltaY)
                return;

            static auto CursorDeltaYHook = safetyhook::create_mid(pCursorDeltaY, [](SafetyHookContext& regs)
            {
                regs.xmm0.f32[0] = CarryRemainder(regs.xmm0.f32[0], fResidualY);
            });
        };
    }
} MenuCursor;
