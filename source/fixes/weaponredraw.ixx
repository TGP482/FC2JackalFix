module;

#include <common.hxx>

export module weaponredraw;

import common;
import dunia;

// Mission giver interiors are Weapon-Safe social regions (enumSocialType 2). Entry signals
// "holster_lock": holster, draw lock set at +0x50. Exit signals "draw_unlock": lock cleared, state
// machine asked to redraw. Mission accept door plays a knock anim on the way out, redraw lost
// against it. Player empty handed until a manual switch, which never reads the lock.
//
// Traced: unlock lands, current weapon -1, lock clears, no CPawnWeaponMgr::Draw until the manual
// switch. Draw from inside the unlock handler refused too. Draw from the next frame took on every
// recorded exit.

// CPawnWeaponMgr. Handles are weapon entity ids, -1 empty hands.
static constexpr ptrdiff_t nWeaponCurrent = 0x14;

// Manager lives here on the signal handler.
static constexpr ptrdiff_t nSignalHandlerMgr = 0x04;

// First frame always sufficed. Rest covers an exit landing mid anim.
static constexpr int nDrawFrames = 60;

// __thiscall on the manager. Equips requested weapon, picks one when none requested, returns early
// once a weapon is in hand.
using Draw_t = void(__fastcall*)(void* pMgr);

static Draw_t Draw = nullptr;

static uint8_t* pPendingMgr = nullptr;
static int nPendingFrames = 0;

static SafetyHookInline DrawUnlockHook{};

static void __fastcall DrawUnlock(uint8_t* pHandler)
{
    DrawUnlockHook.fastcall(pHandler);

    pPendingMgr = *reinterpret_cast<uint8_t**>(pHandler + nSignalHandlerMgr);
    nPendingFrames = nDrawFrames;
}

static void Tick()
{
    if (pPendingMgr == nullptr || Draw == nullptr)
        return;

    if (nPendingFrames-- <= 0 || *reinterpret_cast<int32_t*>(pPendingMgr + nWeaponCurrent) != -1)
    {
        pPendingMgr = nullptr;
        return;
    }

    Draw(pPendingMgr);
}

class WeaponRedraw
{
public:
    WeaponRedraw()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            Draw = reinterpret_cast<Draw_t>(dunia_find("83 EC 0C 53 55 56 8B F1 8B 46 18 3B 05 ? ? ? ? 57 75 05"));

            // Lock and unlock writers sit back to back, differing only in the stored byte.
            if (auto* pUnlock = dunia_find("8B 41 04 C6 40 50 00 C3"))
                DrawUnlockHook = safetyhook::create_inline(pUnlock, DrawUnlock);

            // CPawnInputListener::Update. debug.ixx owns +0x16, so this takes the entry.
            if (auto* pInputPass = dunia_input_pass())
            {
                static auto TickHook = safetyhook::create_mid(pInputPass, [](SafetyHookContext&)
                {
                    Tick();
                });
            }
        };
    }
} WeaponRedraw;
