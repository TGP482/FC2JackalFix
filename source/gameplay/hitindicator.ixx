module;

#include <common.hxx>
#include <atomic>

export module hitindicator;

import common;
import dunia;
import settings;

// Pattern anchor to loop head, and to Hit_Borders.
static constexpr uintptr_t nLoopHeadOffset = 14;
static constexpr uintptr_t nHitBordersOffset = 0x75;

static std::atomic<bool> bNoHitIndicator{ false };

static uintptr_t nHitBordersReturn = 0;
static SafetyHookMid PlayerDamagedHook{};

// Jumps to the loop's exhausted path, so the refcount release still runs.
static void SkipHitBorders(SafetyHookContext& regs)
{
    if (bNoHitIndicator.load(std::memory_order_relaxed))
        regs.eip = nHitBordersReturn;
}

class HitIndicator
{
public:
    HitIndicator()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Hook sits between the position read and the loop setup.
            auto pattern = dunia_pattern("F3 0F 10 00 F3 0F 10 48 04 F3 0F 10 50 08 33 FF 8D 83 20 02 00 00");
            if (pattern.empty())
                return;

            nHitBordersReturn = reinterpret_cast<uintptr_t>(pattern.get_first(nHitBordersOffset));

            // Installed either way so the toggle flips live.
            PlayerDamagedHook = safetyhook::create_mid(pattern.get_first(nLoopHeadOffset), SkipHitBorders);

            BindBool(bNoHitIndicator, PREF_NOHITINDICATOR);
        };
    }
} HitIndicator;
