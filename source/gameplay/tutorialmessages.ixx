module;

#include <common.hxx>

export module tutorialmessages;

import common;
import dunia;
import settings;

static constexpr int32_t SKIP_NONE = 0;
static constexpr int32_t SKIP_POPUPS = 1;
static constexpr int32_t SKIP_FLOATING = 2;

// Consulted by the hooks, which stay installed either way, so the ini is live.
static int32_t nSkipTutorials = SKIP_NONE;

// CGameMessageBoxHelper::ActiveFloatingId.
static constexpr uintptr_t HELPER_ACTIVEFLOATINGID = 0x18;

// The id the manager writes when nothing is up.
static constexpr uint32_t MSGBOXID_NONE = 0xFFFFFFFF;

// Domino register. Handle comes back in EDX:EAX, id low and a constant 2 high.
using DominoRegister_t = uint64_t(__fastcall*)(void* pRegistry, void* pEdx, void* pObject, const char* pszMethod);

// Domino invoke. The handle goes back as two arguments, which must stay adjacent on the stack:
// the function takes the address of the first to rebuild the key.
using DominoInvoke_t = void(__fastcall*)(void* pRegistry, void* pEdx, uint32_t nId, uint32_t nType, const void* pArgument);

// Domino release.
using DominoRelease_t = void(__fastcall*)(void* pRegistry, void* pEdx, uint32_t nId, uint32_t nType);

// __thiscall on CGuiMessageBoxManager, the id by pointer.
using HideMessageBox_t = void(__fastcall*)(void* pManager, void* pEdx, const uint32_t* pnId);

static void** ppDominoRegistry = nullptr;
static void** ppMessageBoxManager = nullptr;

static DominoRegister_t DominoRegister = nullptr;
static DominoInvoke_t DominoInvoke = nullptr;
static DominoRelease_t DominoRelease = nullptr;
static HideMessageBox_t HideMessageBox = nullptr;

// MSVC8 std::string: proxy, sixteen byte buffer, length, capacity. Zero length at +0x14 selects
// the one argument call and leaves buffer and capacity unread.
struct DuniaString
{
    void* pProxy;
    char szBuffer[16];
    uint32_t nSize;
    uint32_t nCapacity;
};
static_assert(sizeof(DuniaString) == 0x1C, "MSVC8 std::string is 0x1C bytes");

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

// HideFloatingTutorialMessageBox without the string parse its script binding does. Leaves
// FloatingMessageTag at helper+0x38 set; only serialisation reads it.
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

// Behind CreateTutorialMessageBox and its WithActionMap twin. __thiscall, six stack args, RET 0x18.
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

// The same one argument wider: a hud.mgb page id ahead of the captions. RET 0x1C.
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

// Returns an MSVC8 std::string by value, so the caller's buffer is the first stack argument. Run
// then taken down rather than skipped: the script keeps the string and its later Hide no-ops.
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
            BindInt(nSkipTutorials, PREF_SKIPTUTORIALS);

            // Popup prologue: +0x2E is the domino registry global, +0x32 the register call.
            auto* pPopup = static_cast<uint8_t*>(dunia_find(
                "81 EC 80 01 00 00 8B 84 24 98 01 00 00 53 55 56 57 50 51 89 4C 24 20 "
                "8B 8C 24 AC 01 00 00 33 DB 3B CB 8B C4 89 08 74 04 83 41 04 01 8B 0D ? ? ? ? E8"));
            if (!pPopup)
                return;

            // The same shape; the frame size and the spill slot tell the two apart.
            auto* pCustom = dunia_find(
                "81 EC 6C 01 00 00 8B 84 24 88 01 00 00 53 55 56 57 50 51 89 4C 24 1C "
                "8B 8C 24 9C 01 00 00 33 DB 3B CB 8B C4 89 08 74 04 83 41 04 01 8B 0D ? ? ? ? E8");
            if (!pCustom)
                return;

            auto* pInvoke = dunia_find("83 EC 24 56 57 8B 79 44 8D 71 34 8D 44 24 30 50 8D 4C 24 0C 51 8B CE E8");
            if (!pInvoke)
                return;

            // Release shares its prologue with other functions, so it is reached through the
            // release loop at the tail of the helper's result handler; the CALL is at +0x0F.
            auto* pRelease = static_cast<uint8_t*>(dunia_find("74 19 8B 46 04 8B 0E 50 51 8B 0D ? ? ? ? E8", 0x0F));
            if (!pRelease)
                return;

            ppDominoRegistry = *reinterpret_cast<void***>(pPopup + 0x2E);
            DominoRegister = reinterpret_cast<DominoRegister_t>(CallTarget(pPopup + 0x32));
            DominoInvoke = reinterpret_cast<DominoInvoke_t>(pInvoke);
            DominoRelease = reinterpret_cast<DominoRelease_t>(CallTarget(pRelease));

            if (!ppDominoRegistry || !DominoRegister || !DominoInvoke || !DominoRelease)
                return;

            TutorialMessageBoxHook = safetyhook::create_inline(pPopup, TutorialMessageBox);
            CustomTutorialMessageBoxHook = safetyhook::create_inline(pCustom, CustomTutorialMessageBox);

            // Floating hints below; the pop-up halves stay installed even if this does not resolve.
            auto* pFloating = dunia_find("81 EC AC 01 00 00 53 55 56 57 8D 44 24 13 89 4C 24 18 50 8D 4C 24 24 33 DB BE");
            if (!pFloating)
                return;

            // The replace half of the same function: manager global at +0x17, hide call at +0x1C.
            auto* pHide = static_cast<uint8_t*>(dunia_find("8B 7C 24 20 8B 57 18 8D 77 18 83 C4 08 3B 15 ? ? ? ? 74 0C 8B 0D ? ? ? ? 56 E8"));
            if (!pHide)
                return;

            ppMessageBoxManager = *reinterpret_cast<void***>(pHide + 0x17);
            HideMessageBox = reinterpret_cast<HideMessageBox_t>(CallTarget(pHide + 0x1C));

            if (!ppMessageBoxManager || !HideMessageBox)
                return;

            FloatingTutorialMessageBoxHook = safetyhook::create_inline(pFloating, FloatingTutorialMessageBox);
        };
    }
} TutorialMessages;
