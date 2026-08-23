module;

#include <common.hxx>

export module mousespeed;

import common;
import dunia;
import settings;

// CActionMapCurveFilter caps mouse output at maxOutput; Dunia's default sentinel lifts the cap.
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
            // After maxOutput loads. The filter is shared with gamepads, so check the type.
            auto* pMaxOutput = dunia_find("F3 0F 10 4F 0C F3 0F 5E C2 F3 0F 59 47 10 F3 0F 59 C4 0F 28 F8 0F 54 FD 0F 2F F9 76", 5);
            if (!pMaxOutput)
                return;

            static auto MouseSpeedCapHook = safetyhook::create_mid(pMaxOutput, [](SafetyHookContext& regs)
            {
                if (!bRemoveMouseSpeedCap || *(uint32_t*)(regs.edi + 4) != nMouseFilterId)
                    return;

                if (regs.xmm1.f32[0] < fNoCap)
                    regs.xmm1.f32[0] = fNoCap;
            });

            BindBool(bRemoveMouseSpeedCap, PREF_MOUSESPEEDCAP);
        };
    }
} MouseSpeed;
