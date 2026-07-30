/*
  "Added 'v' as a dedicated keyboard button to look back when driving" from Boggalog's
  Far Cry 2 Patched, in code. Credit to Boggalog for the feature.

  Stock Far Cry 2 already looks behind while driving, but only while both mouse buttons are held.
  The guide adds a keyboard equivalent by editing config/inputactionmapcommon.xml inside patch.dat.

  The engine side already works. CPawnInputListener::OnSignal (FUN_10144760) dispatches on a crc32
  of the signal name (table at 0x10F93A68, CMP EAX, [0x10F93A84] for crc32("look_back")) and its
  look_back arm is look_pov with a fixed axis: SetPovAxis (FUN_10144630) with {0, -1, 0} (the -1.0f
  at 0x10E0CC4C) and count 2. SetPovAxis clears the look accumulators at this+0x10 and this+0x14,
  then writes:

    pov = *(pawn+0x10) + 0x140;
    pov[0x68] = -0.0f - x;
    pov[0x6C] = y;                                  ; -1.0 here is "looking backwards"
    if (engaged) { pov[0x38..0x4C] = 0; pov[0x05] |= 2; }

  Nothing in that path is mouse-specific and nothing in it tests for a vehicle. The stock feature is
  mouse-only and driving-only because the binding sits in the common_in_vehicle action map, so only
  the binding is missing: calling SetPovAxis on key edges is cheaper than fabricating an input event
  or handing the parser a rewritten config file.

  Hook is CPawnInputListener::Update (FUN_10144AA0), vtable 0x10E20F00 slot 3, the once-per-frame
  pass that turns accumulated input into camera and aim state: CALL FUN_101449B0 integrates look
  delta * frame delta into aim, then CALL FUN_10143820 skips on-foot look in a vehicle or on a
  mounted gun. The work runs before the trampoline, since the accumulator clear would otherwise
  throw away deltas the frame had already consumed.

  Vehicle test is FUN_100E7330, which resolves the pawn's entity link 0xFD1BA782 and casts it to the
  class registered as "CVehicle" at 0x1007EB10. FUN_10143820 makes the same call two instructions
  later for the same reason, so this is the engine's own in-vehicle predicate.

  The key does not appear in the in-game controls menu, which is built from defaultusercontrols.xml,
  so rebinding still needs the archive edit.
*/

module;

#include <common.hxx>

export module lookback;

import common;
import dunia;

// CPawnInputListener.
static constexpr uintptr_t nListenerPawn = 0x20;

// Axis vector SetPovAxis takes. The count is an integer despite sitting among floats (CMP dword
// ptr [EDI+0xC], 2) and must be at least 2 or the call is dropped.
struct PovAxis
{
    float fX;
    float fY;
    float fZ;
    uint32_t nAxisCount;
};

// What the engine's look_back arm passes, plus the recentre matching the stock binding's release.
static constexpr PovAxis LookBehind = { 0.0f, -1.0f, 0.0f, 2 };
static constexpr PovAxis LookForward = { 0.0f, 0.0f, 0.0f, 2 };

// The guide's key. Not exposed as a setting; the game has no way to show or rebind it.
static constexpr int nLookBackKey = 'V';

// __thiscall with one stack argument.
using SetPovAxis_t = void(__fastcall*)(void* pListener, void* pEdx, const PovAxis* pAxis);

// __cdecl, one argument, returns the vehicle or null.
using GetVehicle_t = void*(__cdecl*)(void* pPawn);

static SetPovAxis_t SetPovAxis = nullptr;
static GetVehicle_t GetVehicle = nullptr;

// Edge triggered, like the press and release pair the stock binding produces.
static bool bLookingBack = false;

// __thiscall with two stack arguments and a callee cleanup of eight bytes.
static SafetyHookInline PawnInputUpdateHook{};

static void __fastcall PawnInputUpdate(void* pListener, void* pEdx, uint32_t nUnused, uint32_t nInputDisabled)
{
    auto* pPawn = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pListener) + nListenerPawn);

    // Engine tests the low byte of this argument and nothing else, so the upper three may be dirty.
    bool bWantLookBack = false;
    if ((nInputDisabled & 0xFF) == 0 && pPawn != nullptr)
        bWantLookBack = GetVehicle(pPawn) != nullptr && (GetAsyncKeyState(nLookBackKey) & 0x8000) != 0;

    if (bWantLookBack != bLookingBack)
    {
        // Skipped when the pawn is gone, since it takes the view with it. The latch updates either
        // way, so a key held through a load does not return as a release with no matching press.
        if (pPawn != nullptr)
            SetPovAxis(pListener, nullptr, bWantLookBack ? &LookBehind : &LookForward);

        bLookingBack = bWantLookBack;
    }

    PawnInputUpdateHook.fastcall(pListener, pEdx, nUnused, nInputDisabled);
}

class LookBack
{
public:
    LookBack()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // CPawnInputListener::SetPovAxis. Entry through the axis count check and the two look
            // accumulator stores.
            auto povPattern = dunia_pattern("83 EC 08 56 57 8B 7C 24 14 83 7F 0C 02 8B F1 0F 82 07 01 00 00 0F 57 C9 F3 0F 11 4E 10 F3 0F 11 4E 14");

            // GetVehicle. Entry through the virtual call that resolves the pawn's entity link.
            auto vehiclePattern = dunia_pattern("83 EC 08 8B 4C 24 0C 8B 01 8B 50 7C 53 FF D2 83 CB FF B9 01 00 00 00");

            // CPawnInputListener::Update. Disabled-input early out through the pawn fetch, stopping
            // before the first call. No absolute addresses in that span.
            auto updatePattern = dunia_pattern("33 C0 38 44 24 08 56 8B F1 74 0A 88 46 04 88 46 05 5E C2 08 00 8B 4E 20 3B C8 74 4A");

            if (povPattern.empty() || vehiclePattern.empty() || updatePattern.empty())
                return;

            SetPovAxis = reinterpret_cast<SetPovAxis_t>(povPattern.get_first());
            GetVehicle = reinterpret_cast<GetVehicle_t>(vehiclePattern.get_first());

            PawnInputUpdateHook = safetyhook::create_inline(updatePattern.get_first(), PawnInputUpdate);

            // No recentre on shutdown; the pawn it would talk to is already gone.
            JackalFix::onShutdownEvent() += []()
            {
                bLookingBack = false;
            };
        };
    }
} LookBack;
