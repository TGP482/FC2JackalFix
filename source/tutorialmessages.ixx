module;

#include <common.hxx>

export module tutorialmessages;

import common;
import dunia;
import settings;

static constexpr int32_t SKIP_NONE = 0;
static constexpr int32_t SKIP_POPUPS = 1;
static constexpr int32_t SKIP_FLOATING = 2;

// The hooks stay installed either way and consult this, so the ini is live.
static int32_t nSkipTutorials = SKIP_NONE;

// CGameMessageBoxHelper, from the property registration in FUN_10706E00.
static constexpr uintptr_t HELPER_ACTIVEFLOATINGID = 0x18;

// The id the manager writes when nothing is up, compared against at 0x10704FA4.
static constexpr uint32_t MSGBOXID_NONE = 0xFFFFFFFF;

// 0x1055D070. Callee cleans eight bytes, so __fastcall with the two arguments behind ECX/EDX is the
// same shape. The handle comes back in EDX:EAX, id low and a constant 2 high.
using DominoRegister_t = uint64_t(__fastcall*)(void* pRegistry, void* pEdx, void* pObject, const char* pszMethod);

// 0x1055D510. The handle goes back in as two separate arguments; the function takes the address of
// the first to rebuild the key, so they have to stay adjacent on the stack.
using DominoInvoke_t = void(__fastcall*)(void* pRegistry, void* pEdx, uint32_t nId, uint32_t nType, const void* pArgument);

// 0x1055C3F0.
using DominoRelease_t = void(__fastcall*)(void* pRegistry, void* pEdx, uint32_t nId, uint32_t nType);

// 0x1004C970. __thiscall on CGuiMessageBoxManager, the id by pointer.
using HideMessageBox_t = void(__fastcall*)(void* pManager, void* pEdx, const uint32_t* pnId);

static void** ppDominoRegistry = nullptr;
static void** ppMessageBoxManager = nullptr;

static DominoRegister_t DominoRegister = nullptr;
static DominoInvoke_t DominoInvoke = nullptr;
static DominoRelease_t DominoRelease = nullptr;
static HideMessageBox_t HideMessageBox = nullptr;

// MSVC8 std::string, laid out as every caller in the module builds it: proxy, the sixteen byte
// buffer, then length and capacity. 0x1055D510 reads the length at +0x14, and a zero there selects
// the one argument call the message box path always makes, leaving the buffer and the capacity
// unread. An empty string is what the button press path passes too.
struct DuniaString
{
    void* pProxy;
    char szBuffer[16];
    uint32_t nSize;
    uint32_t nCapacity;
};
static_assert(sizeof(DuniaString) == 0x1C, "MSVC8 std::string is 0x1C bytes");

// Reads the rel32 of a CALL and returns what it points at.
static void* CallTarget(uint8_t* pCall)
{
    return pCall + 5 + *reinterpret_cast<int32_t*>(pCall + 1);
}

// Answers the script's Event_Continue without a box ever existing.
static void FireContinue(void* pObject, const char* pszMethod)
{
    if (!ppDominoRegistry || !pObject || !pszMethod)
        return;

    auto* pRegistry = *ppDominoRegistry;
    if (!pRegistry)
        return;

    auto nHandle = DominoRegister(pRegistry, nullptr, pObject, pszMethod);
    auto nId = static_cast<uint32_t>(nHandle);
    auto nType = static_cast<uint32_t>(nHandle >> 32);

    // Zeroed length, so the handler is called with self alone.
    DuniaString Argument{};
    Argument.nCapacity = 15;

    DominoInvoke(pRegistry, nullptr, nId, nType, &Argument);
    DominoRelease(pRegistry, nullptr, nId, nType);
}

// HideFloatingTutorialMessageBox (0x107050F0) without the string parse its script binding does to
// get here. FloatingMessageTag at helper+0x38 is left as it was; the engine's hide clears it, but
// it is read only by serialisation and the next floating box overwrites it.
static void HideActiveFloating(void* pHelper)
{
    if (!pHelper || !ppMessageBoxManager)
        return;

    auto* pManager = *ppMessageBoxManager;
    if (!pManager)
        return;

    auto& nId = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(pHelper) + HELPER_ACTIVEFLOATINGID);
    if (nId == MSGBOXID_NONE)
        return;

    HideMessageBox(pManager, nullptr, &nId);
    nId = MSGBOXID_NONE;
}

static SafetyHookInline TutorialMessageBoxHook{};
static SafetyHookInline CustomTutorialMessageBoxHook{};
static SafetyHookInline FloatingTutorialMessageBoxHook{};

// 0x107060A0, behind CreateTutorialMessageBox and its WithActionMap twin. __thiscall on the helper,
// six stack arguments, callee cleans 0x18. The plain binding passes "" for the action map.
static void __fastcall TutorialMessageBox(void* pHelper, void* pEdx, const char* pszTitle, const char* pszText,
    const char* pszButton, const char* pszActionMap, void* pObject, const char* pszMethod)
{
    if (nSkipTutorials < SKIP_POPUPS)
    {
        TutorialMessageBoxHook.fastcall(pHelper, pEdx, pszTitle, pszText, pszButton, pszActionMap, pObject, pszMethod);
        return;
    }

    FireContinue(pObject, pszMethod);
}

// 0x10705B30, the same thing one argument wider: a hud.mgb page id ahead of the captions. Callee
// cleans 0x1C.
static void __fastcall CustomTutorialMessageBox(void* pHelper, void* pEdx, const char* pszPage, const char* pszTitle,
    const char* pszText, const char* pszButton, const char* pszActionMap, void* pObject, const char* pszMethod)
{
    if (nSkipTutorials < SKIP_POPUPS)
    {
        CustomTutorialMessageBoxHook.fastcall(pHelper, pEdx, pszPage, pszTitle, pszText, pszButton, pszActionMap, pObject, pszMethod);
        return;
    }

    FireContinue(pObject, pszMethod);
}

// 0x10704CF0. Returns an MSVC8 std::string by value, so the caller's buffer is the first stack
// argument and the declared ones follow it. This one files no callback record: its box has no
// button and the script hides it by hand, so nothing can stall. Run as normal and taken back down
// afterwards rather than skipped, because the return value is a live string the script keeps. Its
// later Hide then finds its stored id no longer matching, no-ops, and pulses Finished as usual.
static void* __fastcall FloatingTutorialMessageBox(void* pHelper, void* pEdx, void* pReturn, const char* pszText,
    const char* pszActionMap, void* pObject, const char* pszMethod)
{
    auto* pResult = FloatingTutorialMessageBoxHook.fastcall<void*>(pHelper, pEdx, pReturn, pszText, pszActionMap, pObject, pszMethod);

    if (nSkipTutorials >= SKIP_FLOATING)
        HideActiveFloating(pHelper);

    return pResult;
}

class TutorialMessages
{
public:
    TutorialMessages()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            nSkipTutorials = JackalFixSettings.GetInt(PREF_SKIPTUTORIALS);

            JackalFix::onIniFileChange() += []()
            {
                nSkipTutorials = JackalFixSettings.GetInt(PREF_SKIPTUTORIALS);
            };

            // 0x107060A0. The prologue through the registry load and the CALL that files the
            // callback record, which is where the registry variable and 0x1055D070 both come from:
            //
            //     81 EC 80 01 00 00   SUB  ESP,0x180
            //     ...
            //     83 41 04 01         ADD  dword ptr [ECX+0x4],0x1   script object addref
            //     8B 0D ? ? ? ?       MOV  ECX,[0x116473E4]          <- +0x2E, the registry
            //     E8 ? ? ? ?          CALL 0x1055D070                <- +0x32
            auto popupPattern = dunia_pattern(
                "81 EC 80 01 00 00 8B 84 24 98 01 00 00 53 55 56 57 50 51 89 4C 24 20 "
                "8B 8C 24 AC 01 00 00 33 DB 3B CB 8B C4 89 08 74 04 83 41 04 01 8B 0D ? ? ? ? E8");
            if (popupPattern.empty())
                return;

            // 0x10705B30. Byte for byte the same shape; the frame size and the spill slot are what
            // tell the two apart.
            auto customPattern = dunia_pattern(
                "81 EC 6C 01 00 00 8B 84 24 88 01 00 00 53 55 56 57 50 51 89 4C 24 1C "
                "8B 8C 24 9C 01 00 00 33 DB 3B CB 8B C4 89 08 74 04 83 41 04 01 8B 0D ? ? ? ? E8");
            if (customPattern.empty())
                return;

            // 0x1055D510.
            auto invokePattern = dunia_pattern("83 EC 24 56 57 8B 79 44 8D 71 34 8D 44 24 30 50 8D 4C 24 0C 51 8B CE E8");
            if (invokePattern.empty())
                return;

            // 0x1055C3F0 is a sixteen byte function whose own prologue is shared, so it is reached
            // through the release loop at the tail of the helper's result handler (0x10707200)
            // instead:
            //
            //     74 19               JZ   done
            //     8B 46 04            MOV  EAX,[ESI+0x4]      record type
            //     8B 0E               MOV  ECX,[ESI]          record id
            //     50 51               PUSH EAX / PUSH ECX
            //     8B 0D ? ? ? ?       MOV  ECX,[0x116473E4]
            //     E8 ? ? ? ?          CALL 0x1055C3F0         <- +0x0F
            auto releasePattern = dunia_pattern("74 19 8B 46 04 8B 0E 50 51 8B 0D ? ? ? ? E8");
            if (releasePattern.empty())
                return;

            auto* pPopup = popupPattern.get_first<uint8_t>(0);
            ppDominoRegistry = *reinterpret_cast<void***>(pPopup + 0x2E);
            DominoRegister = reinterpret_cast<DominoRegister_t>(CallTarget(pPopup + 0x32));
            DominoInvoke = reinterpret_cast<DominoInvoke_t>(invokePattern.get_first(0));
            DominoRelease = reinterpret_cast<DominoRelease_t>(CallTarget(releasePattern.get_first<uint8_t>(0x0F)));

            if (!ppDominoRegistry || !DominoRegister || !DominoInvoke || !DominoRelease)
                return;

            TutorialMessageBoxHook = safetyhook::create_inline(pPopup, TutorialMessageBox);
            CustomTutorialMessageBoxHook = safetyhook::create_inline(customPattern.get_first(0), CustomTutorialMessageBox);

            // Floating hints from here down, independent of the above: the pop-up halves stay
            // installed even if this one does not resolve.

            // 0x10704CF0.
            auto floatingPattern = dunia_pattern("81 EC AC 01 00 00 53 55 56 57 8D 44 24 13 89 4C 24 18 50 8D 4C 24 24 33 DB BE");
            if (floatingPattern.empty())
                return;

            // The replace half of the same function, where it takes down whatever floating box was
            // already up. Both the manager global and 0x1004C970 are read off it:
            //
            //     8B 7C 24 20         MOV  EDI,[ESP+0x20]     the helper
            //     8B 57 18            MOV  EDX,[EDI+0x18]     helper->ActiveFloatingId
            //     8D 77 18            LEA  ESI,[EDI+0x18]
            //     83 C4 08            ADD  ESP,0x8
            //     3B 15 ? ? ? ?       CMP  EDX,[0x10E2C55C]   the invalid id
            //     74 0C               JZ   nothing up
            //     8B 0D ? ? ? ?       MOV  ECX,[0x10FDED3C]   <- +0x17, the manager
            //     56                  PUSH ESI
            //     E8 ? ? ? ?          CALL 0x1004C970         <- +0x1C
            auto hidePattern = dunia_pattern("8B 7C 24 20 8B 57 18 8D 77 18 83 C4 08 3B 15 ? ? ? ? 74 0C 8B 0D ? ? ? ? 56 E8");
            if (hidePattern.empty())
                return;

            auto* pHide = hidePattern.get_first<uint8_t>(0);
            ppMessageBoxManager = *reinterpret_cast<void***>(pHide + 0x17);
            HideMessageBox = reinterpret_cast<HideMessageBox_t>(CallTarget(pHide + 0x1C));

            if (!ppMessageBoxManager || !HideMessageBox)
                return;

            FloatingTutorialMessageBoxHook = safetyhook::create_inline(floatingPattern.get_first(0), FloatingTutorialMessageBox);
        };
    }
} TutorialMessages;
