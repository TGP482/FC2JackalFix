module;

#include <common.hxx>

export module mousespeed;

import common;
import dunia;
import settings;

// CActionMapCurveFilter caps mouse output at maxOutput. Restoring Dunia's default sentinel removes
// the cap without affecting the response curve.
static constexpr float fNoCap = 1000000.0f;
static constexpr uint32_t nMouseFilterId = 0xDE7832DA; // written to filter+0x04 by the ctor at 0x107D8B40

static bool bRemoveMouseSpeedCap = true;

class MouseSpeed
{
public:
    MouseSpeed()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Hooked after maxOutput is loaded, so the cap can be raised. The filter is shared
            // with gamepads, so the input type is checked first.
            auto pattern = dunia_pattern("F3 0F 10 4F 0C F3 0F 5E C2 F3 0F 59 47 10 F3 0F 59 C4 0F 28 F8 0F 54 FD 0F 2F F9 76");
            if (pattern.empty())
                return;

            static auto MouseSpeedCapHook = safetyhook::create_mid(pattern.get_first(5), [](SafetyHookContext& regs)
            {
                if (!bRemoveMouseSpeedCap || *(uint32_t*)(regs.edi + 4) != nMouseFilterId)
                    return;

                if (regs.xmm1.f32[0] < fNoCap)
                    regs.xmm1.f32[0] = fNoCap;
            });

            static auto MouseSpeedCB = []()
            {
                bRemoveMouseSpeedCap = JackalFixSettings.GetInt(PREF_MOUSESPEEDCAP) != 0;
            };

            MouseSpeedCB();

            JackalFix::onIniFileChange() += []()
            {
                MouseSpeedCB();
            };
        };
    }
} MouseSpeed;
