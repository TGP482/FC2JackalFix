module;

#include <common.hxx>
#include <atomic>
#include <vector>

export module hudfadeout;

import common;
import dunia;
import settings;

// CFCXMainHudUI. One countdown per widget group, topped up to fadeOutDelay by anything that wants
// the HUD seen; each group's state machine hides once its own countdown runs out.
static constexpr ptrdiff_t nFadeDelay = 0x250;
static constexpr ptrdiff_t nAmmoState = 0xAC;
static constexpr ptrdiff_t nGrenadeState = 0xB0;
static constexpr uint32_t  nStateShown = 2;

// Into each countdown read, past the last top-up of the tick so nothing writes over what we set.
static constexpr uintptr_t nAmmoReadOffset = 7;
static constexpr uintptr_t nGrenadeReadOffset = 8;

// The health bar keeps no state machine: a per-tick flag decides between topping its own countdown
// up and letting it run out. DL carries that flag, CL the freeze that zeroes the countdown.
static constexpr uintptr_t nHealthFlagOffset = 7;

// Drawing a new weapon plays the grenade group's show animation itself, around the state machine.
static constexpr uintptr_t nDrawTimerOffset = 8;
static constexpr uintptr_t nDrawSkipOffset = 0x53;

static constexpr int nAlwaysHidden = -2;
static constexpr int nAlwaysVisible = -1;

// Never counts down to zero within a session.
static constexpr float fNeverFades = 1.0e9f;

// Under any frame time, so the next tick takes the hide path.
static constexpr float fHideNow = 1.0e-6f;

static std::atomic<int> nHudFadeOut{ 0 };
static uintptr_t nDrawSkip = 0;

// What the HUD data shipped, read before the first write over it.
static float fStockDelay = 0.0f;

// EBX is the group's countdown, ESI the HUD.
static void ApplyFadeOut(SafetyHookContext& regs, ptrdiff_t nState)
{
    auto* pDelay = reinterpret_cast<float*>(static_cast<uintptr_t>(regs.esi) + nFadeDelay);
    auto* pTimer = reinterpret_cast<float*>(static_cast<uintptr_t>(regs.ebx));

    if (fStockDelay == 0.0f)
        fStockDelay = *pDelay;

    const auto nMode = nHudFadeOut.load(std::memory_order_relaxed);

    switch (nMode)
    {
    case nAlwaysHidden:
        // Zero holds a hidden group down; a group still up needs one tick left to run its hide.
        *pTimer = *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(regs.esi) + nState) == nStateShown ? fHideNow : 0.0f;
        break;

    case nAlwaysVisible:
        *pTimer = fNeverFades;
        break;

    default:
        *pDelay = nMode > 0 ? static_cast<float>(nMode) : fStockDelay;

        // A shortened delay bites this tick rather than waiting for the next top-up.
        if (*pTimer > *pDelay)
            *pTimer = *pDelay;
        break;
    }
}

static void ApplyToAmmo(SafetyHookContext& regs) { ApplyFadeOut(regs, nAmmoState); }
static void ApplyToGrenades(SafetyHookContext& regs) { ApplyFadeOut(regs, nGrenadeState); }

static void ApplyToHealth(SafetyHookContext& regs)
{
    const auto nMode = nHudFadeOut.load(std::memory_order_relaxed);
    if (nMode >= 0)
        return;

    *reinterpret_cast<uint8_t*>(&regs.edx) = nMode == nAlwaysVisible;
    *reinterpret_cast<uint8_t*>(&regs.ecx) = nMode == nAlwaysHidden;
}

static void ApplyToWeaponDraw(SafetyHookContext& regs)
{
    if (nHudFadeOut.load(std::memory_order_relaxed) == nAlwaysHidden)
        regs.eip = nDrawSkip;
}

class HudFadeOut
{
public:
    HudFadeOut()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // The ammo counter, then the grenade group. Each reads its countdown in both arms of its
            // state machine, so two matches apiece and no more.
            auto ammo = dunia_pattern("C6 86 73 02 00 00 00 F3 0F 10 03");
            auto grenades = dunia_pattern("53 8B CE E8 ? ? ? ? F3 0F 10 03");

            auto health = dunia_pattern("C6 86 64 02 00 00 00 84 D2 74");
            auto draw = dunia_pattern("F3 0F 11 86 54 02 00 00 F3 0F 10 86 58 02 00 00");

            // The ammo counter fades in from more than the state machine, so its two fade-in entries
            // are stubbed out wholesale rather than chased one caller at a time. Both take only this,
            // so a bare return balances.
            auto bullets = dunia_pattern("83 EC 0C 56 8B F1 57 8B 7E 04 85 FF 0F 84 BE 00 00 00 E8 ? ? ? ? "
                                         "83 F8 03 0F 87 84 00 00 00 FF 24 85 ? ? ? ? 8B 46 08");
            auto rockets = dunia_pattern("56 8B F1 E8 ? ? ? ? 8B 4E 2C 85 C9 74 19 6A 00 6A 01 6A 00");

            if (ammo.empty() || grenades.empty() || health.empty() || draw.empty() || bullets.empty() || rockets.empty())
                return;

            nDrawSkip = reinterpret_cast<uintptr_t>(draw.get_first(nDrawSkipOffset));

            static std::vector<safetyhook::MidHook> Hooks;
            for (size_t i = 0; i < ammo.size(); i++)
                Hooks.emplace_back(safetyhook::create_mid(ammo.get(i).get<void>(nAmmoReadOffset), ApplyToAmmo));

            for (size_t i = 0; i < grenades.size(); i++)
                Hooks.emplace_back(safetyhook::create_mid(grenades.get(i).get<void>(nGrenadeReadOffset), ApplyToGrenades));

            Hooks.emplace_back(safetyhook::create_mid(health.get_first(nHealthFlagOffset), ApplyToHealth));
            Hooks.emplace_back(safetyhook::create_mid(draw.get_first(nDrawTimerOffset), ApplyToWeaponDraw));

            static raw_mem fnBulletFadeIn(bullets.get_first(), { 0xC3 });
            static raw_mem fnRocketFadeIn(rockets.get_first(), { 0xC3 });

            ApplyAndWatch([]()
            {
                const auto nMode = JackalFixSettings.GetInt(PREF_HUDFADEOUT);
                nHudFadeOut = nMode;
                fnBulletFadeIn.Set(nMode == nAlwaysHidden);
                fnRocketFadeIn.Set(nMode == nAlwaysHidden);
            });
        };
    }
} HudFadeOut;
