module;

#include <common.hxx>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

export module custombinds;

import common;
import dunia;
import settings;
import inputdevice;

// Four extra control rows on the Controls page, and an Escape out of the "press a key" capture
// screen.
//
//     Inspect    plays the pawn's own idle fiddle animation on demand
//     Holster    puts the weapon away the way the engine's own holstering does
//     Walk       halves the pawn's max speed
//     Look Back  writes the POV pair the two mouse buttons write
//
// Inspect and Holster also answer a one second pad hold, of reload and of any one d-pad direction.

// Gates the four effects where each is used, so it stops them at once. The rows are gated at the
// parse instead, which the page's Back handler re-runs on every exit. Bindings are never gated, so
// a saved rebind survives the setting being off.
static bool bCustomBinds = true;

enum class CustomBind { Inspect, Holster, Walk, LookBack, COUNT };

static constexpr size_t nBindCount = static_cast<size_t>(CustomBind::COUNT);

// Written from the signal dispatcher, polled elsewhere.
static std::atomic<bool> bBindDown[nBindCount]{};
static std::atomic<bool> bBindPressed[nBindCount]{};

// Pressing one of these while holstered has to draw by hand: the equip sync at 0x10161DAA and
// EquipItemByName at 0x1012D6D0 both bail on the holster latch. Equipping the item holstered
// instead was a dead end. The one latch free item equip, 0x1012D730, clears the equipped kind and
// loses the weapon the latch holds.
static constexpr const char* WeaponSelectSignals[] =
{
    "select_next_weapon", "select_previous_weapon", "select_hand_to_hand_weapon",
    "select_secondary_weapon", "select_primary_weapon", "select_special_weapon",
    "cycleweapon", "select_next_throw_gadget", "toggle_gadget",
};

static uint32_t nWeaponSelectSignal[std::size(WeaponSelectSignals)]{};

// Consumed in the input pass, the only place with the pawn in hand.
static std::atomic<bool> bWeaponSelectPressed{};

static bool IsCustomBindDown(CustomBind bind)
{
    return bCustomBinds && bBindDown[static_cast<size_t>(bind)].load();
}

static bool ConsumeCustomBindPress(CustomBind bind)
{
    return bCustomBinds && bBindPressed[static_cast<size_t>(bind)].exchange(false);
}

// The engine hashes every name it compares by id with this, at 0x10229400. Bit exact zlib CRC-32:
// reflected 0xEDB88320, init and final complement. Verified against debug.ixx's "show_pausemenu"
// 0x04127107.
static uint32_t NameId(std::string_view sName)
{
    static constexpr uint32_t nPolynomial = 0xEDB88320;

    static const auto table = []()
    {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            auto c = i;
            for (int nBit = 0; nBit < 8; ++nBit)
                c = (c & 1) ? (nPolynomial ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();

    uint32_t nCrc = 0xFFFFFFFF;
    for (auto ch : sName)
        nCrc = (nCrc >> 8) ^ table[static_cast<uint8_t>(static_cast<uint8_t>(ch) ^ nCrc)];

    return ~nCrc;
}

// One control row, 0x74 bytes. +0x00 and +0x1C are the label and key text, +0x38 the input pair the
// player bound and +0x40 the default, +0x50 the actionmap name, +0x6C group, +0x70 conflictmask.
static constexpr ptrdiff_t nElementLabel = 0x00;

// MSVC8 std::wstring: buffer inline at +0x04 while capacity is 7, size at +0x14, capacity at +0x18.
static constexpr ptrdiff_t nStringBuffer = 0x04;
static constexpr ptrdiff_t nStringSize = 0x14;
static constexpr ptrdiff_t nStringCapacity = 0x18;
static constexpr uint32_t nWideShortCapacity = 7;

// CActionMap. Bindings are a plain array; the name id is the one turretexitbind reads.
static constexpr ptrdiff_t nMapNameId = 0x6C;

// SBinding, 0x14 bytes, zero initialised so the capacity at +0x08 and the imported flag at +0x10
// stay clear.
static constexpr ptrdiff_t nBindingInputs = 0x00;
static constexpr ptrdiff_t nBindingInputCount = 0x04;
static constexpr ptrdiff_t nBindingSignal = 0x0C;
static constexpr size_t nBindingStride = 0x14;

// The input descriptors a binding points at: an axis flag, then the device, key and action ids
// that <Binding input= action=> hashes.
static constexpr ptrdiff_t nInputDevice = 0x04;
static constexpr ptrdiff_t nInputKey = 0x08;
static constexpr ptrdiff_t nInputAction = 0x0C;
static constexpr size_t nInputStride = 0x10;

// Frame slots in the DefaultUserControls parser at the hook, measured from the post prologue ESP.
static constexpr ptrdiff_t nParsePage = 0x8C;
static constexpr ptrdiff_t nParseCategoryId = 0x90;
static constexpr ptrdiff_t nParseCategoryName = 0x70;

// Frame slots in the <Binding> handler once AddBinding has returned and cleaned its two arguments.
static constexpr ptrdiff_t nBindingParseMap = 0x50;
static constexpr ptrdiff_t nBindingParseBinding = 0x34;

// Each row takes its category's group, 1 on foot and 2 in a vehicle, and the mask its neighbours
// carry, so it clashes with the same set they do and the page offers the usual swap.
static constexpr int nConflictMask = 12;

// Keyboard controls take the 0x108813E0 branch; 0 is the pad one.
static constexpr int nKeyboardControl = 1;

// A binding the parser has already built is the template: cloning the whole descriptor keeps the
// axis flag and anything else in it, and matching its ids proves the hashing before a single write.
struct AnchorDesc
{
    const char* pszMap;
    const char* pszKey;
    const char* pszSignal;
};

struct BindDesc
{
    const char* pszControl;     // Control name=, and the oasisstrings "Actions" key with no entry
    // No insert into that table was found, so 0x108811C0's miss is overwritten with this instead.
    // English only, in every language.
    const wchar_t* pszLabel;
    const char* pszKey;         // Control key1=
    // A rebind writes <ActionMap name="..."><MassRename><Rename hexInput hexToInput> into
    // %APPDATA%InputUserActionMap.xml and the filter rewrites raw input before bindings match, so a
    // new control does nothing until a map importing this *_remap holds a binding on its key.
    const char* pszActionMap;
    const char* pszCategory;    // the Category it joins
    const char* pszSignalDown;
    const char* pszSignalUp;
    AnchorDesc anchor;          // in the map that imports pszActionMap
    int nControlGroup;          // Control group=, its category's
};

static constexpr BindDesc Binds[nBindCount] =
{
    { "jackalfix_inspect", L"Inspect Weapon", "kb:i",    "common_weapons_remap", "CATEGORY_WEAPONS",
      "jackalfix_inspect", "jackalfix_inspect_release", { "common_weapons", "4", "select_special_weapon" }, 1 },
    // Not kb:h: healing is bound there and takes the weapon out of the hands and puts it back, so
    // the two cancel each other. kb:x is free in defaultusercontrols and every inputactionmap.
    { "jackalfix_holster", L"Holster Weapon", "kb:x",    "common_weapons_remap", "CATEGORY_WEAPONS",
      "jackalfix_holster", "jackalfix_holster_release", { "common_weapons", "4", "select_special_weapon" }, 1 },
    { "jackalfix_walk",    L"Walk",    "kb:lalt", "common_move_remap",    "CATEGORY_MOVEMENTS",
      "jackalfix_walk",    "jackalfix_walk_release",    { "common_move",    "lshift", "lock_sprint" }, 1 },
    // look_back lives in common_in_vehicle alone, and common_in_vehicle_remap is imported by that
    // one map, so a rebind cannot leak into common_passenger or common_driving. toggle_awd is the
    // anchor because it appears in no other map, unlike exitvehicle.
    { "jackalfix_lookback", L"Look Back", "kb:v", "common_in_vehicle_remap", "CATEGORY_VEHICLES",
      "jackalfix_lookback", "jackalfix_lookback_release",
      { "common_in_vehicle", "r", "toggle_awd" },            2 },
};

// __thiscall on the options page. Returns the element it appended.
using AddControl_t = void*(__thiscall*)(void* pPage, uint32_t nCategory, const char* pszName,
    const char* pszKey, const char* pszActionMap, int nGroup, int nConflictMask, int bKeyboard);

// __thiscall on the map. The flag is the <Binding secondary=> sense: set, an equal input merges
// into the existing binding and takes its signal over. Clear appends, which is the only safe one
// here since a rebind can land our key on top of a stock one.
using AddBinding_t = void(__thiscall*)(void* pMap, const void* pBinding, int bPrimary);

static AddControl_t AddControl = nullptr;
static AddBinding_t AddBinding = nullptr;

static uint32_t nDeviceKeyboard = 0;
static uint32_t nKeyEscape = 0;
static uint32_t nActionPress = 0;
static uint32_t nActionRelease = 0;

static uint32_t nSignalDown[nBindCount]{};
static uint32_t nSignalUp[nBindCount]{};
static uint32_t nAnchorMap[nBindCount]{};
static uint32_t nAnchorKey[nBindCount]{};
static uint32_t nAnchorSignal[nBindCount]{};
static uint32_t nControlKey[nBindCount]{};

// Every rebind reloads the action maps into fresh objects, so comparing map pointers both stops a
// second file redefining the map from doubling the binding and lets the reload put it back.
static void* pBoundMap[nBindCount]{};

static void SetWideString(uintptr_t pString, const wchar_t* pText)
{
    auto nCapacity = *reinterpret_cast<uint32_t*>(pString + nStringCapacity);
    auto* pBuffer = (nCapacity > nWideShortCapacity)
        ? *reinterpret_cast<wchar_t**>(pString + nStringBuffer)
        : reinterpret_cast<wchar_t*>(pString + nStringBuffer);

    if (pBuffer == nullptr)
        return;

    auto nLength = std::wcslen(pText);

    // The oasisstrings miss text is longer than any label here, so this never trims.
    if (nLength > nCapacity)
        return;

    for (size_t i = 0; i <= nLength; i++)
        pBuffer[i] = pText[i];

    *reinterpret_cast<uint32_t*>(pString + nStringSize) = static_cast<uint32_t>(nLength);
}

static void AddCustomBinding(void* pMap, const uint8_t* pTemplate, uint32_t nKey, uint32_t nAction,
    uint32_t nSignal)
{
    uint8_t input[nInputStride];
    std::memcpy(input, pTemplate, sizeof(input));
    *reinterpret_cast<uint32_t*>(input + nInputKey) = nKey;
    *reinterpret_cast<uint32_t*>(input + nInputAction) = nAction;

    // AddBinding copies the descriptors into the map's own storage, so a stack array is enough.
    uint8_t binding[nBindingStride]{};
    *reinterpret_cast<const uint8_t**>(binding + nBindingInputs) = input;
    *reinterpret_cast<uint32_t*>(binding + nBindingInputCount) = 1;
    *reinterpret_cast<uint32_t*>(binding + nBindingSignal) = nSignal;

    AddBinding(pMap, binding, 0);
}

class CustomBinds
{
public:
    CustomBinds()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            BindBool(bCustomBinds, PREF_CUSTOMBINDS);

            nDeviceKeyboard = NameId("kb");
            nKeyEscape = NameId("escape");
            nActionPress = NameId("press");
            nActionRelease = NameId("release");

            for (size_t i = 0; i < nBindCount; i++)
            {
                nSignalDown[i] = NameId(Binds[i].pszSignalDown);
                nSignalUp[i] = NameId(Binds[i].pszSignalUp);
                nAnchorMap[i] = NameId(Binds[i].anchor.pszMap);
                nAnchorKey[i] = NameId(Binds[i].anchor.pszKey);
                nAnchorSignal[i] = NameId(Binds[i].anchor.pszSignal);

                // key1 is "kb:<key>", and only the key half is compared against a binding input.
                std::string_view sKey = Binds[i].pszKey;
                auto nColon = sKey.find(':');
                nControlKey[i] = NameId(nColon == std::string_view::npos ? sKey : sKey.substr(nColon + 1));
            }

            for (size_t i = 0; i < std::size(WeaponSelectSignals); i++)
                nWeaponSelectSignal[i] = NameId(WeaponSelectSignals[i]);

            AddControl = reinterpret_cast<AddControl_t>(
                dunia_find("83 EC 74 56 57 8D 84 24 80 00 00 00 50 81 C1 94 01 00 00 E8"));

            AddBinding = reinterpret_cast<AddBinding_t>(
                dunia_find("83 EC 1C 80 7C 24 24 00 53 55 56 57 89 4C 24 14 C6 44 24 13 00"));

            InstallControls();
            InstallBindings();
            InstallSignals();
            InstallCaptureExit();
        };
    }

private:
    // Tail of the per Category <Control> loop in the DefaultUserControls parser at 0x10827160.
    // AddControl appends into the map<uint32, array of 0x74> at page+0x194, keyed by CRC-32 of the
    // *localised* category label, and the frame here still holds the page, that key and the name.
    //
    //     10827623  83 C5 01     ADD EBP,0x1          <- control index
    //     10827626  FF D0        CALL EAX             <- child count
    //     1082762f  0F 8C ...    JL 108274E2          <- back to the next Control
    //     10827635               CMP [ESP+0x88],0x10  <- hook, category name string about to go
    static void InstallControls()
    {
        auto* pLoopEnd = dunia_find(
            "83 C5 01 FF D0 3B E8 BF 0F 00 00 00 0F 8C ? ? ? ? 83 BC 24 88 00 00 00 10", 18);

        if (pLoopEnd == nullptr || AddControl == nullptr)
            return;

        static auto ControlsHook = safetyhook::create_mid(pLoopEnd, [](SafetyHookContext& regs)
        {
            if (!bCustomBinds)
                return;

            auto* pPage = *reinterpret_cast<void**>(regs.esp + nParsePage);
            auto nCategory = *reinterpret_cast<uint32_t*>(regs.esp + nParseCategoryId);

            char szCategory[64]{};
            ReadDuniaString(regs.esp + nParseCategoryName, szCategory, sizeof(szCategory));

            if (pPage == nullptr || szCategory[0] == '\0')
                return;

            for (const auto& bind : Binds)
            {
                if (std::strcmp(szCategory, bind.pszCategory) != 0)
                    continue;

                auto* pElement = AddControl(pPage, nCategory, bind.pszControl, bind.pszKey,
                    bind.pszActionMap, bind.nControlGroup, nConflictMask, nKeyboardControl);

                if (pElement != nullptr)
                    SetWideString(reinterpret_cast<uintptr_t>(pElement) + nElementLabel, bind.pszLabel);
            }
        });
    }

    // The <Binding> handler, at the return from CActionMap::AddBinding.
    //
    //     105860c8  8B 4C 24 58  MOV ECX,[ESP+0x58]   <- the map
    //     105860cc  E8 ...       CALL AddBinding
    //     105860d1  8D 54 24 34  LEA EDX,[ESP+0x34]   <- hook
    static void InstallBindings()
    {
        auto* pBindingParsed = dunia_find(
            "38 5C 24 4C 8D 4C 24 34 0F 94 C0 50 51 8B 4C 24 58 E8 ? ? ? ? 8D 54 24 34", 22);

        if (pBindingParsed == nullptr || AddBinding == nullptr)
            return;

        static auto BindingsHook = safetyhook::create_mid(pBindingParsed, [](SafetyHookContext& regs)
        {
            auto* pMap = *reinterpret_cast<uint8_t**>(regs.esp + nBindingParseMap);
            auto* pBinding = reinterpret_cast<uint8_t*>(regs.esp + nBindingParseBinding);

            if (pMap == nullptr)
                return;

            auto* pInput = *reinterpret_cast<uint8_t**>(pBinding + nBindingInputs);
            if (pInput == nullptr || *reinterpret_cast<uint32_t*>(pBinding + nBindingInputCount) != 1)
                return;

            // A compound or pad input is no template, and the anchors are all single key presses.
            if (*reinterpret_cast<uint32_t*>(pInput + nInputDevice) != nDeviceKeyboard)
                return;
            if (*reinterpret_cast<uint32_t*>(pInput + nInputAction) != nActionPress)
                return;

            auto nMap = *reinterpret_cast<uint32_t*>(pMap + nMapNameId);
            auto nKey = *reinterpret_cast<uint32_t*>(pInput + nInputKey);
            auto nSignal = *reinterpret_cast<uint32_t*>(pBinding + nBindingSignal);

            for (size_t i = 0; i < nBindCount; i++)
            {
                if (nMap != nAnchorMap[i] || nKey != nAnchorKey[i] || nSignal != nAnchorSignal[i])
                    continue;
                if (pBoundMap[i] == pMap)
                    continue;

                pBoundMap[i] = pMap;
                AddCustomBinding(pMap, pInput, nControlKey[i], nActionPress, nSignalDown[i]);
                AddCustomBinding(pMap, pInput, nControlKey[i], nActionRelease, nSignalUp[i]);
            }
        });
    }

    // The listener debug.ixx refuses three signals in, hooked five bytes past its entry so the two
    // hooks cannot land on the same bytes. Nothing is pushed yet, so ESP+4 is the signal id.
    static void InstallSignals()
    {
        auto* pDispatch = dunia_find("83 EC 50 A8 01 53 55 56 57 8B F9");
        if (pDispatch == nullptr)
            return;

        static auto SignalHook = safetyhook::create_mid(pDispatch, [](SafetyHookContext& regs)
        {
            auto* pSignal = *reinterpret_cast<uint32_t**>(regs.esp + 4);
            if (pSignal == nullptr)
                return;

            auto nSignal = *pSignal;

            for (size_t i = 0; i < nBindCount; i++)
            {
                if (nSignal == nSignalDown[i])
                {
                    bBindDown[i] = true;
                    bBindPressed[i] = true;
                }
                else if (nSignal == nSignalUp[i])
                {
                    bBindDown[i] = false;
                }
            }

            for (auto nSelect : nWeaponSelectSignal)
            {
                if (nSignal == nSelect)
                {
                    bWeaponSelectPressed = true;
                    break;
                }
            }
        });
    }

    // Escape needs both halves. The capture listener at 0x10823E20 dropped (kb, escape) before it
    // could reach the commit, so the screen had no way out:
    //
    //     10823eff  B2 01        MOV DL,0x1           <- drop flag, NOPed here
    //
    // The apply pass at 0x10823130 then returns 0 for Escape anyway, since 0x1087FE70 rejects any
    // pair outside the bindable input table, so the hook reports success without applying it.
    //
    // The same press also reaches the page's Back handler at 0x10827770 through the UI. Its own
    // modal guard cannot gate that: the input pump runs every filter, the capture listener among
    // them, before the UI translate, so page+0x1B4 is already zero by then. Hence the mark.
    static constexpr uint8_t nNop = 0x90;

    // Both passes are one frame, so the mark only has to outlive the input pump. Bounding it stops
    // a commit with no Back behind it from eating the next Escape instead.
    static constexpr uint64_t nEscapeMarkLife = 250;

    static void InstallCaptureExit()
    {
        auto* pApply = dunia_find(
            "81 EC 60 09 00 00 53 55 56 8B F1 8B 8C 24 70 09 00 00 8B 41 38 8B 51 3C");

        auto* pEscapeDrop = static_cast<uint8_t*>(dunia_find(
            "8B 06 3B 44 24 10 75 0B 8B 4E 04 3B 4C 24 14 75 02 B2 01", 17));

        if (pApply == nullptr || pEscapeDrop == nullptr)
            return;

        ApplyBindingHook = safetyhook::create_inline(pApply, ApplyBinding);

        injector::WriteMemory<uint8_t>(pEscapeDrop, nNop, true);
        injector::WriteMemory<uint8_t>(pEscapeDrop + 1, nNop, true);

        // Anchored on the prologue and the page+0x1C8 test, which no other page shares.
        auto* pPageBack = dunia_find("81 EC 28 01 00 00 53 55 8B E9 33 DB 38 9D C8 01 00 00 56 57 0F 84");
        if (pPageBack == nullptr)
            return;

        PageBackHook = safetyhook::create_inline(pPageBack, PageBack);
    }

    static inline SafetyHookInline ApplyBindingHook{};
    static inline SafetyHookInline PageBackHook{};

    static inline void* pCapturePage = nullptr;
    static inline uint64_t nEscapeMark = 0;

    // The conflict flag is set only by the interactive capture; the load pass at 0x10826990
    // replays saved binds with it clear and must keep working.
    static char __fastcall ApplyBinding(void* pPage, void* pEdx, void* pElement, const uint32_t* pPair,
        char bCheckConflict)
    {
        if (bCheckConflict != 0 && pPair != nullptr && pPair[0] == nDeviceKeyboard && pPair[1] == nKeyEscape)
        {
            pCapturePage = pPage;
            nEscapeMark = GetTickCount64();
            return 1;
        }

        return ApplyBindingHook.fastcall<char>(pPage, pEdx, pElement, pPair, bCheckConflict);
    }

    // Swallows the one Back the closing press would otherwise raise. Any other Back, a later
    // Escape or the nav bar prompt, runs stock.
    static void __fastcall PageBack(void* pPage, void* pEdx)
    {
        auto nMark = nEscapeMark;
        nEscapeMark = 0;

        if (pPage == pCapturePage && nMark != 0 && GetTickCount64() - nMark <= nEscapeMarkLife)
            return;

        PageBackHook.fastcall<void>(pPage, pEdx);
    }
} CustomBinds;

// CPawnInputListener::Update at the second pawn fetch, clear of debug.ixx's hook at +0x16 and
// checkpointrespawn's at +0x0F. Reached on every frame the first fetch found a pawn, and ECX is
// that pawn, loaded three bytes earlier:
//
//     10144AD8  MOV  ECX,[ESI+0x20]
//     10144ADB  CALL 1007E1B0      <- hook
static constexpr ptrdiff_t nInputPassPawn = 0x35;

// Pawn+0x10 is the state block inputtoggles reads. The archetype pass at 0x10082160 registers the
// state driver at +0x4F8 and the weapon manager at +0x4F0 in it.
static constexpr ptrdiff_t nPawnStateBlock = 0x10;
static constexpr ptrdiff_t nStateDriver = 0x4F8;
static constexpr ptrdiff_t nWeaponManager = 0x4F0;

static constexpr uint64_t nPadHoldTime = 1000;

static uintptr_t StateBlock(uintptr_t nPawn)
{
    return nPawn == 0 ? 0 : *reinterpret_cast<uintptr_t*>(nPawn + nPawnStateBlock);
}

// Inspect. The idle fiddle is CPawn's IdleCycleBreaker: its tick at 0x10080B40 counts the state
// block's fMinTime 0x4B4 and fMaxTime 0x4B8 down through a timer at +0x4BC, then sends one signal
// to the state driver. The state machine drops that signal wherever it has no transition, so
// nothing here tests what the player is doing.
using SendSignal_t = void(__thiscall*)(void* pDriver, uint32_t nSignal);

static SendSignal_t SendSignal = nullptr;

// Lifted from the fire block rather than written down. 0xD3724593 in both builds; the guard hash
// two branches up in the same tick is crc32("IdleCycleBreaker"), 0x1D238520, which is what pinned
// these as plain crc32 name ids.
static uint32_t nIdleBreakSignal = 0;

// The fire block, on the lazy init of the static name id.
//
//     C7 05 10FDFA38 D3724593   MOV [nameId],hash
//     8B 15 10FDFA38            MOV EDX,[nameId]
//     51 8B 4C 24 14 8B C4 8910 push the id, ECX = the driver
//     E8 ? ? ? ?                CALL SendSignal
static constexpr ptrdiff_t nSignalImmediate = 6;
static constexpr ptrdiff_t nSendSignalCall = 25;

// Into CPawnInputListener::OnSignal, at the join past the pressed gate and the null-pawn check.
// EAX is the incoming signal name id.
static constexpr ptrdiff_t nSignalHandlerCheck = 0x27;

// crc32("reload"), which inputactionmapcommon.xml binds kb:r and pad:x to. It is bound
// action="press" only, so there is no release signal to hook and the hold has to end on the button
// mask instead.
static constexpr uint32_t nReloadSignal = 0xA2B8CE55;

static uint64_t nReloadPressTime = 0;
static uint16_t nReloadButtons = 0;
static bool bReloadFired = false;

static void PlayInspect(uintptr_t nPawn)
{
    auto nBlock = StateBlock(nPawn);
    if (SendSignal == nullptr || nBlock == 0)
        return;

    SendSignal(reinterpret_cast<void*>(nBlock + nStateDriver), nIdleBreakSignal);
}

// The whole button mask at the press, so letting go of anything held alongside reload also ends the
// hold, and a rebound reload button needs no extra work.
static void OnReloadPressed()
{
    auto nButtons = GetPadState().nButtons;

    nReloadButtons = nButtons;
    nReloadPressTime = (IsPadActiveDevice() && nButtons != 0) ? GetTickCount64() : 0;
    bReloadFired = false;
}

static bool ReloadHoldElapsed()
{
    if (nReloadPressTime == 0)
        return false;

    if (!IsPadActiveDevice() || (GetPadState().nButtons & nReloadButtons) != nReloadButtons)
    {
        nReloadPressTime = 0;
        bReloadFired = false;
        return false;
    }

    if (bReloadFired || GetTickCount64() - nReloadPressTime < nPadHoldTime)
        return false;

    bReloadFired = true;
    return true;
}

// Holster. The "HolsterWeapon" script command at 0x109F85E0 is not the animated path: it clears
// the equipped kind outright and the weapon vanishes.
//
// The engine's own holstering moves a latch on the equip state and sends one signal to the state
// driver; the animation asserts the latch again from its own "holster_lock" event. Deep water at
// 0x100A0B10 is the shape:
//
//     100A0BA2  CALL 101260C0        ; [[manager+4]+0x50] = 1
//     100A0C34  MOV  [x],C64DABBF
//     100A0C55  CALL 1029DA10        ; SendSignal, ECX = the driver
//     100A0C79  CALL 101260D0        ; [[manager+4]+0x50] = 0
//     100A0C98  MOV  [x],08AC6FEA
//
// 0x101260E0 reads that latch back: the engine's own record of whether the weapon is away.

// The manager keeps its equip state one level down, at +0x04.
static constexpr ptrdiff_t nEquipState = 0x04;

// What is in the player's hands right now: 0 nothing, 1 weapon, 2 item. +0x78 is the queued one,
// which is what the draw restores from, so nothing here needs it.
static constexpr ptrdiff_t nEquippedKind = 0x74;
static constexpr int32_t nKindWeapon = 1;
static constexpr int32_t nKindItem = 2;

// The latch 0x101260C0 sets, 0x101260D0 clears and 0x101260E0 reads.
static constexpr ptrdiff_t nEquipHolstered = 0x50;

// Sent after the latch moves. The draw one is crc32("drawweapon"); the holster one is unnamed, and
// 0x100A0C34, 0x106A784E and 0x106E9B08 are the three sites that send it. Plain name ids, so both
// builds share them the way nReloadSignal does.
static constexpr uint32_t nHolsterSignal = 0xC64DABBF;
static constexpr uint32_t nDrawSignal = 0x08AC6FEA;

// XINPUT_GAMEPAD_DPAD_UP/DOWN/LEFT/RIGHT.
static constexpr uint16_t nDpadMask = 0x000F;

static uint16_t nDpadHeld = 0;
static uint64_t nDpadHeldSince = 0;
static bool bDpadFired = false;

// True on the frame a single direction reaches nPadHoldTime. Nothing here touches the pad state the
// drivers hand on, so the d-pad keeps its normal action.
static bool DpadHoldElapsed()
{
    if (!IsPadActiveDevice())
    {
        nDpadHeld = 0;
        return false;
    }

    auto nDpad = static_cast<uint16_t>(GetPadState().nButtons & nDpadMask);

    // One direction at a time, so a diagonal is not a hold.
    if ((nDpad & (nDpad - 1)) != 0)
        nDpad = 0;

    if (nDpad != nDpadHeld)
    {
        nDpadHeld = nDpad;
        nDpadHeldSince = GetTickCount64();
        bDpadFired = false;
        return false;
    }

    if (nDpad == 0 || bDpadFired || GetTickCount64() - nDpadHeldSince < nPadHoldTime)
        return false;

    bDpadFired = true;
    return true;
}

static void ToggleHolster(uintptr_t nPawn)
{
    auto nBlock = StateBlock(nPawn);
    if (SendSignal == nullptr || nBlock == 0)
        return;

    auto nEquip = *reinterpret_cast<uintptr_t*>(nBlock + nWeaponManager + nEquipState);
    if (nEquip == 0)
        return;

    // Read live: a toggle flag of our own would fight the engine's own holstering.
    auto* pHolstered = reinterpret_cast<uint8_t*>(nEquip + nEquipHolstered);
    auto* pDriver = reinterpret_cast<void*>(nBlock + nStateDriver);

    if (*pHolstered != 0)
    {
        *pHolstered = 0;
        SendSignal(pDriver, nDrawSignal);
        return;
    }

    // Nothing in the hands to put away, and the state machine has no transition for it either.
    auto nKind = *reinterpret_cast<int32_t*>(nEquip + nEquippedKind);
    if (nKind != nKindWeapon && nKind != nKindItem)
        return;

    *pHolstered = 1;
    SendSignal(pDriver, nHolsterSignal);
}

// The draw half of ToggleHolster, for a weapon change made while the weapon is away. The change
// itself is left alone and lands once the latch is clear.
static void DrawIfHolstered(uintptr_t nPawn)
{
    auto nBlock = StateBlock(nPawn);
    if (SendSignal == nullptr || nBlock == 0)
        return;

    auto nEquip = *reinterpret_cast<uintptr_t*>(nBlock + nWeaponManager + nEquipState);
    if (nEquip == 0)
        return;

    auto* pHolstered = reinterpret_cast<uint8_t*>(nEquip + nEquipHolstered);
    if (*pHolstered == 0)
        return;

    *pHolstered = 0;
    SendSignal(reinterpret_cast<void*>(nBlock + nStateDriver), nDrawSignal);
}

// Walk. Every locomotion mode reaches one join at 0x10144322, where XMM0 is the mode's max speed,
// so halving it there covers swimming too. Scaling the move axes at [ESI+8] and [ESI+0xC] instead
// is not equivalent: the sprint branch renormalises the direction, and the sprint entry gate reads
// those same fields. Walk beats sprint by redirecting the gate at 0x10144094 to the not-sprinting
// join.
static constexpr float fWalkSpeedScale = 0.5f;

// The gate spills the current state block at [ESP+0x24] and the join reads its stance at +0xA4.
static constexpr uintptr_t nCurrentStateSpill = 0x24;

// Distance from the gate to the not-sprinting join, i.e. the JZ at +0x0E and its 0x3F displacement.
static constexpr ptrdiff_t nNoSprintJoinOffset = 0x4F;

// Look back. OnSignal answers stock look_back at 0x10144828 by building {0, -1, 0, 2} and calling
// the POV setter look_pov shares, 0x10144630, which writes DesiredData +0x68 and +0x6C and ORs bit
// 2 into +0x05. There is no look_back release binding: look_pov sends (0, 0) when either button
// comes up, so the same call with zeros is the whole way back and this must be a hold.
using SetPovOffset_t = void(__thiscall*)(void* pListener, const void* pAxis);

static SetPovOffset_t SetPovOffset = nullptr;

// x, y, one unused float, then the axis count. The setter returns without touching anything below 2,
// and the count is an integer despite sitting in a float payload.
struct PovAxis
{
    float fX;
    float fY;
    float fUnused;
    uint32_t nCount;
};

static_assert(sizeof(PovAxis) == 0x10);

static constexpr PovAxis PovBehind{ 0.0f, -1.0f, 0.0f, 2 };
static constexpr PovAxis PovAhead{ 0.0f, 0.0f, 0.0f, 2 };

static bool bLookBackApplied = false;

class CustomBindActions
{
public:
    CustomBindActions()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            InstallInspect();

            SetPovOffset = reinterpret_cast<SetPovOffset_t>(dunia_find(
                "83 EC 08 56 57 8B 7C 24 14 83 7F 0C 02 8B F1 0F 82 ? ? ? ? 0F 57 C9 "
                "F3 0F 11 4E 10 F3 0F 11 4E 14"));

            InstallTick();
            InstallWalk();
        };
    }

private:
    static void InstallInspect()
    {
        auto fireBlock = dunia_pattern("C7 05 ? ? ? ? 93 45 72 D3 8B 15 ? ? ? ? 51 8B 4C 24 14 8B C4 89 10 E8");
        if (fireBlock.empty())
            return;

        auto nFire = reinterpret_cast<uintptr_t>(fireBlock.get_first());
        auto nCall = nFire + nSendSignalCall;

        nIdleBreakSignal = *reinterpret_cast<uint32_t*>(nFire + nSignalImmediate);
        SendSignal = reinterpret_cast<SendSignal_t>(nCall + 5 + *reinterpret_cast<int32_t*>(nCall + 1));

        // The reload hold is the only thing this one carries, so it does not gate Inspect.
        auto signalHandler = dunia_pattern("8B 44 24 08 83 EC 10 80 78 04 00 56 57 8B F1 0F 84 ? ? ? ? 8B 7C 24 1C 8B 07 3B 05 ? ? ? ? 0F 84 ? ? ? ? 8B 4E 20 85 C9");
        if (signalHandler.empty())
            return;

        static auto ReloadHook = safetyhook::create_mid(signalHandler.get_first(nSignalHandlerCheck), [](SafetyHookContext& regs)
        {
            if (bCustomBinds && regs.eax == nReloadSignal)
                OnReloadPressed();
        });
    }

    static void InstallTick()
    {
        auto* pInputPass = dunia_input_pass();
        if (pInputPass == nullptr)
            return;

        static auto InputPassHook = safetyhook::create_mid(static_cast<uint8_t*>(pInputPass) + nInputPassPawn, [](SafetyHookContext& regs)
        {
            auto nPawn = static_cast<uintptr_t>(regs.ecx);
            auto* pListener = reinterpret_cast<void*>(regs.esi);

            // Every hold timer runs every frame, so a bind press does not stall them.
            auto bInspectHold = ReloadHoldElapsed();
            auto bHolsterHold = DpadHoldElapsed();

            // Before the toggle, so a Holster press on the same frame still ends holstered.
            if (bCustomBinds && bWeaponSelectPressed.exchange(false))
                DrawIfHolstered(nPawn);

            if (ConsumeCustomBindPress(CustomBind::Inspect) || (bCustomBinds && bInspectHold))
                PlayInspect(nPawn);

            if (ConsumeCustomBindPress(CustomBind::Holster) || (bCustomBinds && bHolsterHold))
                ToggleHolster(nPawn);

            // Written every frame rather than on the edge, since nothing here owns DesiredData and a
            // state change between frames would leave the view stuck behind.
            auto bLookBack = IsCustomBindDown(CustomBind::LookBack);

            if (SetPovOffset != nullptr && pListener != nullptr && (bLookBack || bLookBackApplied))
            {
                SetPovOffset(pListener, bLookBack ? &PovBehind : &PovAhead);
                bLookBackApplied = bLookBack;
            }
        });
    }

    static void InstallWalk()
    {
        // Sprint gate: TEST [EDI+4],0x40 / MOV EBX,[ESP+0x24] / JNZ / TEST [EBX+4],0x40 / JZ.
        // The redirect skips the MOV, so EBX is loaded here instead.
        auto sprintGate = dunia_pattern("F6 47 04 40 8B 5C 24 24 75 06 F6 43 04 40 74 3F F3 0F 10 46 0C");
        if (!sprintGate.empty())
        {
            static auto nNoSprintJoin = reinterpret_cast<uintptr_t>(sprintGate.get_first(nNoSprintJoinOffset));

            static auto SprintGateHook = safetyhook::create_mid(sprintGate.get_first(0), [](SafetyHookContext& regs)
            {
                if (!IsCustomBindDown(CustomBind::Walk))
                    return;

                regs.ebx = *reinterpret_cast<uint32_t*>(regs.esp + nCurrentStateSpill);
                regs.eip = nNoSprintJoin;
            });
        }

        // Speed join: XMM0 is the mode's max speed, XMM1 to XMM3 the move direction about to be
        // scaled by it. Anchored on the AND that clears the sprint request bit.
        if (auto* p = dunia_find("80 67 04 BF 8B 54 24 24 8A 42 04 C0 E8 06 24 01 3A C3 F3 0F 59 D8"))
        {
            static auto WalkSpeedHook = safetyhook::create_mid(p, [](SafetyHookContext& regs)
            {
                if (IsCustomBindDown(CustomBind::Walk))
                    regs.xmm0.f32[0] *= fWalkSpeedScale;
            });
        }
    }
} CustomBindActions;
