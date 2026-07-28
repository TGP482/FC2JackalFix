/*
  "Added 'v' as a dedicated keyboard button to look back when driving" from Boggalog's
  Far Cry 2 Patched, in code. Credit to Boggalog for the feature.

  Stock Far Cry 2 can already look behind you while driving, but only by holding both mouse buttons
  at once. The guide's version adds a keyboard equivalent by editing config/inputactionmapcommon.xml
  inside patch.dat, declaring a keyboard CompoundInput named look_pov bound to v and wiring its
  press and release to the look_pov signal.

  What that reveals, once the engine side is read, is that there was never anything to add. The
  signal handler CPawnInputListener::OnSignal (FUN_10144760) dispatches on a CRC32 of the signal
  name - the table of them sits at 0x10F93A68 - and its look_back arm is nothing but look_pov with
  a fixed axis:

    CMP    EAX, [0x10F93A84]      ; crc32("look_back")
    JNZ    next
    MOVSS  XMM1, [0x10E0CC4C]     ; -1.0f
    PXOR   XMM0, XMM0
    MOV    dword [ESP+0x18], 2    ; axis count
    MOVSS  [ESP+0x0C], XMM0       ; x =  0.0
    MOVSS  [ESP+0x10], XMM1       ; y = -1.0
    MOVSS  [ESP+0x14], XMM0       ; z =  0.0
    CALL   FUN_10144630           ; SetPovAxis - the same function look_pov calls

  So look_back is syntactic sugar for SetPovAxis({0, -1, 0}, 2), and SetPovAxis is where the state
  actually lives:

    if (count < 2) return;
    this->lookDeltaX = 0; this->lookDeltaY = 0;
    pov = *(pawn+0x10) + 0x140;
    pov[0x68] = -0.0f - x;
    pov[0x6C] = y;                                  ; -1.0 here is "looking backwards"
    if (engaged) { pov[0x38..0x4C] = 0; pov[0x05] |= 2; }

  Nothing in that path is mouse-specific, and nothing in it tests for a vehicle - the stock feature
  is mouse-only and driving-only purely because the binding sits in the common_in_vehicle action
  map. Which means the engine implementation is complete and only the binding is missing, and the
  cheapest honest way to add one is to call SetPovAxis directly on key edges rather than fabricate
  an input event or hand the parser a rewritten config file.

  The hook goes on CPawnInputListener::Update (FUN_10144AA0), vtable 0x10E20F00 slot 3 - the
  once-per-frame pass that turns accumulated input into camera and aim state. It runs on the same
  object, the same thread and the same phase of the frame the engine uses, and hands over both the
  listener and the pawn in registers:

    XOR   EAX, EAX
    CMP   byte [ESP+8], AL        ; "input disabled"
    MOV   ESI, ECX                ; this
    JZ    enabled
    ...
    MOV   ECX, [ESI+0x20]         ; pawn
    ...
    CALL  FUN_101449B0            ; look delta * frame delta, integrated into aim
    CALL  FUN_10143820            ; skip on-foot look if in a vehicle or on a mounted gun

  The work happens before the trampoline, because SetPovAxis clears the look accumulators at
  this+0x10 and this+0x14 and doing that after the original would throw away deltas the frame had
  already consumed.

  Vehicle test: FUN_100E7330, which resolves the pawn's entity link 0xFD1BA782 and casts it to the
  class registered as "CVehicle" at 0x1007EB10. It is the engine's own in-vehicle test - the same
  call FUN_10143820 makes two instructions later, for the same reason - so nothing here invents a
  predicate the game does not already trust.

  One thing the code route cannot reproduce: the key does not appear in the in-game controls menu,
  because that list is built from defaultusercontrols.xml. The behaviour is identical, but rebinding
  it needs the archive edit.
*/

module;

#include <common.hxx>

export module lookback;

import common;
import dunia;

// CPawnInputListener.
static constexpr uintptr_t nListenerPawn = 0x20;

// The axis vector SetPovAxis takes. The count is an integer despite sitting among floats - the
// function compares it with CMP dword ptr [EDI+0xC], 2 - and must be at least 2 or the call is
// dropped on the floor.
struct PovAxis
{
    float fX;
    float fY;
    float fZ;
    uint32_t nAxisCount;
};

// What the engine's own look_back arm passes: straight behind, and the recentre that matches the
// stock binding's release.
static constexpr PovAxis LookBehind = { 0.0f, -1.0f, 0.0f, 2 };
static constexpr PovAxis LookForward = { 0.0f, 0.0f, 0.0f, 2 };

// The guide's key. Not exposed as a setting: without the entry in defaultusercontrols.xml the game
// has no way to show or rebind it, so a second, invisible binding elsewhere would only confuse.
static constexpr int nLookBackKey = 'V';

// __thiscall with one stack argument.
using SetPovAxis_t = void(__fastcall*)(void* pListener, void* pEdx, const PovAxis* pAxis);

// __cdecl, one argument, returns the vehicle or null.
using GetVehicle_t = void*(__cdecl*)(void* pPawn);

static SetPovAxis_t SetPovAxis = nullptr;
static GetVehicle_t GetVehicle = nullptr;

// Edge triggered, exactly like the press and release pair the stock binding produces. Holding the
// key does not re-send, and letting go recentres once.
static bool bLookingBack = false;

// __thiscall with two stack arguments and a callee cleanup of eight bytes.
static SafetyHookInline PawnInputUpdateHook{};

static void __fastcall PawnInputUpdate(void* pListener, void* pEdx, uint32_t nUnused, uint32_t nInputDisabled)
{
    auto* pPawn = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pListener) + nListenerPawn);

    // The engine tests the low byte of this argument and nothing else, so the upper three are not
    // assumed to be clean.
    bool bWantLookBack = false;
    if ((nInputDisabled & 0xFF) == 0 && pPawn != nullptr)
        bWantLookBack = GetVehicle(pPawn) != nullptr && (GetAsyncKeyState(nLookBackKey) & 0x8000) != 0;

    if (bWantLookBack != bLookingBack)
    {
        // A pawn that has gone away takes the view with it, so the latch is reset rather than
        // carried across the transition - otherwise a key held through a load would come back as a
        // release that never had a matching press.
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

            // CPawnInputListener::Update. Entry through the disabled-input early out and the pawn
            // fetch, stopping before the first call. No absolute addresses in that span.
            auto updatePattern = dunia_pattern("33 C0 38 44 24 08 56 8B F1 74 0A 88 46 04 88 46 05 5E C2 08 00 8B 4E 20 3B C8 74 4A");

            if (povPattern.empty() || vehiclePattern.empty() || updatePattern.empty())
                return;

            SetPovAxis = reinterpret_cast<SetPovAxis_t>(povPattern.get_first());
            GetVehicle = reinterpret_cast<GetVehicle_t>(vehiclePattern.get_first());

            PawnInputUpdateHook = safetyhook::create_inline(updatePattern.get_first(), PawnInputUpdate);

            // The view is left where the player put it on shutdown rather than recentred, because
            // by then the pawn this would be talking to is already gone.
            JackalFix::onShutdownEvent() += []()
            {
                bLookingBack = false;
            };
        };
    }
} LookBack;
