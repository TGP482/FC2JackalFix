/*
  The red wedge that swings round to point at whatever just hit you.
*/

module;

#include <common.hxx>
#include <atomic>

export module hitindicator;

import common;
import dunia;
import settings;

// From the pattern's anchor to the loop head, and to the Hit_Borders block at 0x10809017.
static constexpr uintptr_t nLoopHeadOffset = 14;
static constexpr uintptr_t nHitBordersOffset = 0x75;

static std::atomic<bool> bNoHitIndicator{ false };

class HitIndicator
{
public:
    HitIndicator()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // 0x10808EB0, anchored across the position reads and into the loop setup, since the
            // hook goes between them:
            //
            //     10808fa2   MOVSS XMM0,[EAX]        source entity position, overriding the stim
            //     10808fb0   XOR   EDI,EDI           slot index
            //     10808fb2   LEA   EAX,[EBX+0x220]   first slot's widget
            auto pattern = dunia_pattern("F3 0F 10 00 F3 0F 10 48 04 F3 0F 10 50 08 33 FF 8D 83 20 02 00 00");
            if (pattern.empty())
                return;

            static uintptr_t nHitBordersReturn = reinterpret_cast<uintptr_t>(pattern.get_first(nHitBordersOffset));

            // The hook goes in whatever the setting says, so the toggle can be flipped live. The
            // jump is the one the loop's own exhausted path makes at 0x10808FD6, with nothing
            // pushed in between, so the event refcount release at 0x10809044 still runs.
            static auto PlayerDamagedHook = safetyhook::create_mid(pattern.get_first(nLoopHeadOffset), [](SafetyHookContext& regs)
            {
                if (bNoHitIndicator.load(std::memory_order_relaxed))
                    regs.eip = nHitBordersReturn;
            });

            BindBool(bNoHitIndicator, PREF_NOHITINDICATOR);
        };
    }
} HitIndicator;
