module;

#include <common.hxx>
#include <cstdio>
#include <cstdarg>
#include <cstring>

export module jackalfixmenu;

import common;
import dunia;
import settings;

// The options menu is not data driven. options.mgb carries the page artwork and the navbar prompts;
// the rows under OPTIONS are built in code. CFCXOptionPage::BuildEntries appends one row per sub
// page (Game, Display, Sound, Network, Controls) and hands each row a delegate holding the target
// page's class id. Activating a row looks that id up in the menu manager's page registry and makes
// whatever it finds the next page.
//
// So a new page is a sixth append plus a page object registered under a class id of our own. No
// .mgb editing, no hex editor, and the row inherits the engine's own highlight, sound and
// transition behaviour because it is an ordinary row.

// Temporary. Writes FC2JackalFixMenu.log next to the plugin so a failure can be placed exactly
// rather than guessed at. Set to 0 once the page is working.
#define JACKALFIX_MENU_LOGGING 1

// ------------------------------------------------------------------------------------------------
// Engine object layout.

static constexpr ptrdiff_t nPageList          = 0x0C;  // the row list widget, resolved from the document
static constexpr ptrdiff_t nPageDocument      = 0x14;  // the page's MAGMA document; widgets are looked up in it
static constexpr ptrdiff_t nPageReady         = 0x164; // set once the page is fit to be opened
static constexpr ptrdiff_t nPageTitle         = 0xF4;  // std::wstring drawn as the page heading
static constexpr ptrdiff_t nPageTitleSize     = 0x104;
static constexpr ptrdiff_t nPageTitleCapacity = 0x108;
static constexpr ptrdiff_t nPageParent        = 0xEC;  // page the navbar's Cancel returns to
static constexpr ptrdiff_t nPageManager       = 0x140; // owning menu manager

static constexpr ptrdiff_t nManagerRegistry   = 0x04;  // class id -> page hash map, inside the manager
static constexpr ptrdiff_t nRegistryEnd       = 0x10;  // the map's end iterator; a miss returns it

// std::wstring leaves its inline buffer for the heap at eight characters.
static constexpr uint32_t nWideStringInlineCapacity = 7;

// Page vtable slots. One and two are the same on every page class in this engine; sixteen is the
// row-clearing entry the stock builders call before repopulating.
static constexpr size_t nClassRecordSlot = 1;
static constexpr size_t nOpenSlot        = 2;
static constexpr size_t nClearRowsSlot   = 16;

// Copied wholesale so the clone keeps every behaviour we are not deliberately changing. The real
// table is twenty six entries; the extra slots are never dispatched and cost a few bytes.
static constexpr size_t nVTableSlots = 40;

// Byte offsets inside the functions resolved by pattern, each named after what it points at.
static constexpr ptrdiff_t nBuildDelegateSize    = 0x0B; // push 7Ch, the delegate allocation size
static constexpr ptrdiff_t nBuildGameClassId     = 0x2F; // push <CFCXOptionGamePage class id>
static constexpr ptrdiff_t nBuildMakeDelegate    = 0x36;
static constexpr ptrdiff_t nBuildAddEntry        = 0xAD;
static constexpr ptrdiff_t nCreateClassId        = 0x16; // push <class id>, the page this template makes
static constexpr ptrdiff_t nCreateRegistryFind   = 0x24;
static constexpr ptrdiff_t nCreatePageSize       = 0x36; // push <sizeof(page)>
static constexpr ptrdiff_t nCreateAlloc          = 0x3A;
static constexpr ptrdiff_t nCreateConstruct      = 0x48;
static constexpr ptrdiff_t nCreateRegistryInsert = 0x83;

// CFCXOptionGamePage::Open is three statements: build the rows, clear a field, tail call the base.
static constexpr ptrdiff_t nOpenBuildRows        = 0x03; // call <build rows>
static constexpr ptrdiff_t nOpenResetFieldDisp   = 0x0A; // disp32 of mov [esi+disp32], 0
static constexpr ptrdiff_t nOpenBaseTailJump     = 0x15; // jmp <base open>

// The stock row builder opens by registering three handlers - previous value, next value, activated
// - against the page's own event dispatcher. Replacing that builder wholesale meant our page had
// none, and the input path dispatches into a null object the moment an arrow is pressed. Everything
// needed to register them is read out of the builder's prologue.
static constexpr ptrdiff_t nBuildHandlerSize     = 0x0E; // push 0Ch, the handler allocation size
static constexpr ptrdiff_t nBuildHandlerInit     = 0x22; // call <handler base ctor>
static constexpr ptrdiff_t nBuildDispatcherDisp  = 0x37; // disp32 of lea ebp,[esi+disp32]
static constexpr ptrdiff_t nBuildDispatcherSlot  = 0x3F; // call <slot for event id>
static constexpr ptrdiff_t nBuildRegisterHandler = 0x46; // call <register>
static constexpr ptrdiff_t nBuildHandlerVTables[]{ 0x29, 0x66, 0x9D }; // mov [edi], <vtable>

// ------------------------------------------------------------------------------------------------
// Engine functions. The __thiscall ones are declared __fastcall with an unused EDX, which is the
// same calling convention with the hidden argument spelled out.

using GameAlloc_t      = void*    (__cdecl*)(size_t nSize, int nUnused);
using PageConstruct_t  = void*    (__fastcall*)(void* pPage);
using PageMethod_t     = void     (__fastcall*)(void* pPage, void* pEdx);
using GetClassRecord_t = uint32_t*(__fastcall*)(void* pPage, void* pEdx);
using RegistryFind_t   = void     (__fastcall*)(void* pRegistry, void* pEdx, void** ppIterator, const uint32_t* pClassId);
using RegistryInsert_t = void**   (__fastcall*)(void* pRegistry, void* pEdx, const uint32_t* pClassId);
using MakeDelegate_t   = void*    (__fastcall*)(void* pDelegate, void* pEdx, void* pOwnerPage, const uint32_t* pClassId, int nUnused, char bSetParent);
using AddEntry_t       = int      (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pText, int bEnabled, void* pDelegate);

// Appends a row carrying a two-value control - the < Yes / No > arrows used across the options
// pages. Internally it is AddEntry plus a control stamped from pTemplate into the page's own
// document, then registered in a map at page+190h. That map is why this only works on pages derived
// from the value-page base: a plain CListMenuPage is 190h bytes and has no such member.
using AddValueRow_t    = void*    (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pLabel, const char* pTemplate,
                                                const char* pWidgetName, const wchar_t* pOnText, const wchar_t* pOffText,
                                                int bEnabled, void* pDelegate);

// Selects the value whose byte matches, by searching the control's own value list.
using SetRowValue_t    = void     (__fastcall*)(void* pControl, void* pEdx, const uint8_t* pValue);
static constexpr size_t nRowSetValueSlot = 13;

static GameAlloc_t      GameAlloc      = nullptr;
static PageConstruct_t  PageConstruct  = nullptr;
static RegistryFind_t   RegistryFind   = nullptr;
static RegistryInsert_t RegistryInsert = nullptr;
static MakeDelegate_t   MakeDelegate   = nullptr;
static AddEntry_t       AddEntry       = nullptr;
static AddValueRow_t    AddValueRow    = nullptr;
static PageMethod_t     BasePageOpen   = nullptr;

// The stock call site pushes the handler, then the event id, then calls both functions in turn -
// each taking a single stack argument, so the slot call consumes the event id and leaves the handler
// in place for the register call. Reading it as one call with two arguments unbalances the stack by
// eight bytes, which is what the runtime check catches.
using HandlerInit_t     = void* (__fastcall*)(void* pHandler, void* pEdx, int nUnused);
using DispatcherSlot_t  = void* (__fastcall*)(void* pDispatcher, void* pEdx, int nEvent);
using RegisterHandler_t = void  (__fastcall*)(void* pSlot, void* pEdx, void* pHandler);

static HandlerInit_t     HandlerInit     = nullptr;
static DispatcherSlot_t  DispatcherSlot  = nullptr;
static RegisterHandler_t RegisterHandler = nullptr;
static uint32_t          HandlerVTables[3]{};
static uint32_t          nHandlerSize    = 0;
static ptrdiff_t         nEventDispatcher = 0;

static uint32_t nPageSize        = 0;
static uint32_t nDelegateSize    = 0;
static ptrdiff_t nPageResetField = 0;

// Every menu page is produced by the same template function, so any instance resolves the shared
// registry helpers. The one that builds the page we clone is picked out by the class id it pushes.
static hook::pattern PageCreators{};
static uint8_t* pBuildEntries = nullptr;
static uint8_t* pPageCreator = nullptr;

// ------------------------------------------------------------------------------------------------
// What we clone, and where its widgets come from.
//
// The page has to derive from the value-page base or AddValueRow has nowhere to register its
// controls, so the clone is a CFCXOptionGamePage rather than the plain CFCXOptionPage the OPTIONS
// list itself uses.
//
// A page's widgets are also not private to it: CListMenuPage hashes the name of its MAGMA page and
// resolves the document from that hash, so two page objects naming the same MAGMA page share one
// document, one list widget and one selected index. The clone therefore names a MAGMA page of its
// own. MAINMENU_OPTIONGAME_PAGE is the console cut of game options: it ships in the PC options.mgb -
// options.mgb.desc carries a navbar block for it - but the PC build only ever names
// MAINMENU_OPTIONGAME_PAGE_PC, and the plain name appears nowhere in Dunia's strings.
//
// Borrowing that layout is also what makes the page look right for free. The rule under the
// heading, the row spacing, the arrow art and every font and point size are layout properties, so
// there is nothing to measure or match by hand.
static const char szStockPageLayout[]     = "MAINMENU_OPTIONGAME_PAGE_PC";
static const char szJackalFixPageLayout[] = "MAINMENU_OPTIONGAME_PAGE";

static const wchar_t szJackalFixPageTitle[] = L"JACKAL FIX";

// Widget template each value row is stamped from.
static const char szRowTemplate[] = "SETTING_LABEL_LIST";

// A value row binds to a widget that already exists in the layout, by name. The name is not ours to
// invent: AddValueRow looks it up, stores the result at control+44h, and silently skips adding the
// on/off choices when the lookup fails - which is exactly why the arrows drew with no Yes or No
// against them. MAINMENU_OPTIONGAME_PAGE names its rows generically, SETTING_1 and SETTING_3 through
// SETTING_10, plus a SETTING_SENSITIVITY slider (there is no SETTING_2).
static constexpr ptrdiff_t nControlNamedWidget = 0x44; // widget found by name; where values are added
static constexpr ptrdiff_t nControlRowList     = 0x38; // the SETTING_LABEL_LIST the rows live in
static constexpr ptrdiff_t nControlRowIndex    = 0x40; // this row's index into that list

// How far into the constructor to look for the layout name. Generous and bounded.
static constexpr ptrdiff_t nCtorScanBytes = 0x600;

// ------------------------------------------------------------------------------------------------
// Our page's identity.
//
// The engine's runtime type record is { const char* name, uint32_t depth, uint32_t nameHash[depth] }
// and a class is identified by the address of the trailing hash slot rather than by the hash. Ours
// is derived from the cloned class's record, so we inherit its ancestry and an is-a test against any
// base still succeeds.

static const char szJackalFixPageClass[] = "CFCXJackalFixOptionPage";

static uint32_t JackalFixClassRecord[24]{};
static const uint32_t* pJackalFixClassId = nullptr;
static const uint32_t* pClonedClassId = nullptr;

// Index one holds the vtable proper; index zero is the RTTI slot that sits just before it.
static uintptr_t JackalFixVTable[nVTableSlots + 1]{};

static void* pJackalFixPage = nullptr;

// ------------------------------------------------------------------------------------------------
// The page's contents. Widget names only have to be unique inside our own document, but they are
// prefixed anyway so they can never be confused with a stock row while debugging.

struct BoolRow
{
    const wchar_t* pLabel;
    const char* pWidgetName;
    Pref nPref;
};

static const BoolRow GameplayRows[]
{
    { L"Limited Saving",    "SETTING_3", PREF_LIMITEDSAVING },
    { L"Console Autosaves", "SETTING_4", PREF_CONSOLEAUTOSAVES },
    { L"No Blinking Items", "SETTING_5", PREF_NOBLINKINGITEMS },
    { L"No Colored Signs",  "SETTING_6", PREF_NOCOLOREDSIGNS },
};

// ------------------------------------------------------------------------------------------------

#if JACKALFIX_MENU_LOGGING
static void JFLog(const char* pFormat, ...)
{
    static auto sPath = GetThisModulePath<std::string>() + "FC2JackalFixMenu.log";

    char szLine[512]{};
    va_list args;
    va_start(args, pFormat);
    auto nLength = vsnprintf(szLine, sizeof(szLine) - 3, pFormat, args);
    va_end(args);
    if (nLength < 0)
        return;

    szLine[nLength++] = '\r';
    szLine[nLength++] = '\n';

    auto hFile = CreateFileA(sPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    DWORD nWritten = 0;
    WriteFile(hFile, szLine, static_cast<DWORD>(nLength), &nWritten, nullptr);
    CloseHandle(hFile);
}
#else
static void JFLog(const char*, ...) {}
#endif

// Everything the dump below walks is engine memory whose shape is inferred rather than known, so it
// is read through here. A wrong guess then costs a null in the log instead of a crash.
static bool IsReadable(const void* pAddress, size_t nSize)
{
    if (pAddress == nullptr)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(pAddress, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;

    auto nEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(pAddress) + nSize <= nEnd;
}

template<typename T>
static T SafeRead(const void* pAddress, ptrdiff_t nOffset = 0)
{
    auto pTarget = static_cast<const uint8_t*>(pAddress) + nOffset;
    return IsReadable(pTarget, sizeof(T)) ? *reinterpret_cast<const T*>(pTarget) : T{};
}

static uint8_t* RelativeTarget(uint8_t* pInstruction, ptrdiff_t nOperand)
{
    return pInstruction + nOperand + 4 + *reinterpret_cast<int32_t*>(pInstruction + nOperand);
}

static uint8_t* CallTarget(uint8_t* pCall)
{
    return RelativeTarget(pCall, 1);
}

// Guards the immediate scanning below: an immediate is only dereferenced once it is known to land
// inside Dunia's own image.
static bool PointsIntoDunia(const void* pAddress)
{
    if (hDunia == nullptr)
        return false;

    auto nBase = reinterpret_cast<uintptr_t>(hDunia);
    auto pDos = reinterpret_cast<PIMAGE_DOS_HEADER>(hDunia);
    auto pNt = reinterpret_cast<PIMAGE_NT_HEADERS>(nBase + pDos->e_lfanew);
    auto nAddress = reinterpret_cast<uintptr_t>(pAddress);

    return nAddress >= nBase && nAddress < nBase + pNt->OptionalHeader.SizeOfImage;
}

// A class id points at the last slot of its record, so the record start is found by stepping back
// until the depth field agrees with the distance walked.
static const uint32_t* FindClassRecord(const uint32_t* pClassId)
{
    for (uint32_t nDepth = 1; nDepth + 3 <= std::size(JackalFixClassRecord); nDepth++)
    {
        auto pRecord = pClassId - (nDepth + 1);
        auto pName = reinterpret_cast<const char*>(static_cast<uintptr_t>(pRecord[0]));

        if (pRecord[1] == nDepth && PointsIntoDunia(pName) && *pName == 'C')
            return pRecord;
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// The overrides our clone carries. Everything else is stock behaviour.

static uint32_t* __fastcall JackalFixGetClassRecord(void*, void*)
{
    return JackalFixClassRecord;
}

// Handlers one, two and three are previous value, next value and activated. They are the stock
// CFCXOptionGamePage ones: they look their row up by the handles the page keeps for its own rows,
// and the constructor leaves every one of those at -1, so against our rows they match nothing and do
// nothing. Being registered at all is what matters - the dispatcher has somewhere to go.
static void RegisterValueHandlers(void* pPage)
{
    if (HandlerInit == nullptr || DispatcherSlot == nullptr || RegisterHandler == nullptr)
        return;

    auto pDispatcher = static_cast<uint8_t*>(pPage) + nEventDispatcher;
    for (size_t i = 0; i < std::size(HandlerVTables); i++)
    {
        auto pHandler = static_cast<uint32_t*>(GameAlloc(nHandlerSize, 0));
        if (pHandler == nullptr)
            continue;

        HandlerInit(pHandler, nullptr, 0);
        pHandler[0] = HandlerVTables[i];
        pHandler[2] = reinterpret_cast<uint32_t>(pPage);

        // Registering replaces whatever was there and releases it, so repeating this on every open
        // mirrors the stock page rather than piling handlers up.
        if (auto pSlot = DispatcherSlot(pDispatcher, nullptr, static_cast<int>(i + 1)))
            RegisterHandler(pSlot, nullptr, pHandler);
    }
}

static void BuildRows(void* pPage)
{
    // AddValueRow looks its widget template up inside the page's document and does not check that
    // there is one, so a missing document is a null dereference several frames down. Refuse rather
    // than crash: an empty page is a symptom, a crash is a puzzle.
    auto pDocument = *reinterpret_cast<void**>(static_cast<uint8_t*>(pPage) + nPageDocument);
    if (pDocument == nullptr)
    {
        JFLog("rows: SKIPPED - the page has no document yet");
        return;
    }

    // Enabled, so it draws in the normal ink rather than the greyed-out ink a disabled row gets.
    // With no delegate behind it, activating it still does nothing.
    // Labels are dynamic rows drawn top to bottom; value widgets are fixed furniture, each drawn on
    // the line the layout puts it on. They only line up when row N is paired with the widget that
    // sits on line N. In MAINMENU_OPTIONGAME_PAGE that is SETTING_1 on line 0, the sensitivity
    // slider on line 1, then SETTING_3 upward - which is why SETTING_2 does not exist.
    //
    // So the heading takes line 0 and a blank row takes line 1, leaving the slider unclaimed and the
    // four settings on lines 2 to 5 against SETTING_3 through SETTING_6.
    auto nHeader = AddEntry(pPage, nullptr, L"GAMEPLAY", 1, nullptr);
    auto nSpacer = AddEntry(pPage, nullptr, L"", 0, nullptr);
    JFLog("rows: document %p, header row %d, spacer row %d", pDocument, nHeader, nSpacer);

    for (const auto& row : GameplayRows)
    {
        // No delegate on a value row: it is toggled in place rather than opening a page. That is
        // also what greys out Accept while one is selected, exactly as on Game Options.
        auto pControl = AddValueRow(pPage, nullptr, row.pLabel, szRowTemplate, row.pWidgetName, L"Yes", L"No", 1, nullptr);
        if (pControl == nullptr)
        {
            JFLog("row: FAILED - \"%s\" produced no control", row.pWidgetName);
            continue;
        }

        // The control holds its two choices as raw bytes, 1 behind the on text and 0 behind the off
        // text, so the current setting picks one by matching.
        uint8_t nValue = JackalFixSettings.GetInt(row.nPref) != 0 ? 1 : 0;
        auto ppVTable = *reinterpret_cast<uintptr_t**>(pControl);
        reinterpret_cast<SetRowValue_t>(ppVTable[nRowSetValueSlot])(pControl, nullptr, &nValue);

        // A non-null bound widget is the whole game: it is what AddValueRow adds the Yes and No
        // choices to, and a failed name lookup leaves it null and the row blank.
        auto pBytes = static_cast<uint8_t*>(pControl);
        JFLog("rows: \"%s\" row %u value %u bound %p%s",
            row.pWidgetName,
            SafeRead<uint32_t>(pBytes, nControlRowIndex),
            nValue,
            SafeRead<void*>(pBytes, nControlNamedWidget),
            SafeRead<void*>(pBytes, nControlNamedWidget) == nullptr ? "  <- NAME NOT FOUND" : "");
    }
}

// Stands in for CFCXOptionGamePage::Open, which builds the stock game rows, clears a field and tail
// calls the base. Ours does the same with its own rows, so everything the base open expects to be
// set up still is.
static void __fastcall JackalFixOpenPage(void* pPage, void* pEdx)
{
    auto pBytes = static_cast<uint8_t*>(pPage);
    auto ppVTable = *reinterpret_cast<uintptr_t**>(pPage);

    JFLog("open: page %p document %p list %p ready %u",
        pPage,
        *reinterpret_cast<void**>(pBytes + nPageDocument),
        *reinterpret_cast<void**>(pBytes + nPageList),
        *(pBytes + nPageReady));

    // CListMenuPage::Open makes the same test before it touches anything. The base open does not,
    // and walking its control map on a page the engine has not brought up yet is a fault - so the
    // guard belongs here too. Doing nothing leaves an empty page; not doing it crashes.
    if (*(pBytes + nPageReady) == 0 || *reinterpret_cast<void**>(pBytes + nPageDocument) == nullptr)
    {
        JFLog("open: SKIPPED - the page is not ready");
        return;
    }

    // Before any row exists, exactly as the stock builder does it.
    RegisterValueHandlers(pPage);

    reinterpret_cast<PageMethod_t>(ppVTable[nClearRowsSlot])(pPage, pEdx);
    BuildRows(pPage);

    *reinterpret_cast<uint32_t*>(pBytes + nPageResetField) = 0;
    BasePageOpen(pPage, pEdx);
}

// ------------------------------------------------------------------------------------------------

// Replaces the page heading. The stock title may be inline or on the heap depending on its length,
// so both cases are handled and the existing buffer is reused whenever it is big enough.
static void SetPageTitle(uint8_t* pPage, const wchar_t* pTitle)
{
    auto& nCapacity = *reinterpret_cast<uint32_t*>(pPage + nPageTitleCapacity);
    auto& nSize = *reinterpret_cast<uint32_t*>(pPage + nPageTitleSize);
    auto pField = reinterpret_cast<wchar_t*>(pPage + nPageTitle);
    auto nLength = static_cast<uint32_t>(wcslen(pTitle));

    auto pBuffer = nCapacity > nWideStringInlineCapacity ? *reinterpret_cast<wchar_t**>(pField) : pField;

    if (nLength <= nCapacity && pBuffer != nullptr)
    {
        wmemcpy(pBuffer, pTitle, nLength + 1);
        nSize = nLength;
        return;
    }

    // Taken from the game's allocator so the string's own destructor can release it.
    auto pHeap = static_cast<wchar_t*>(GameAlloc((nLength + 1) * sizeof(wchar_t), 0));
    if (pHeap == nullptr)
        return;

    wmemcpy(pHeap, pTitle, nLength + 1);
    *reinterpret_cast<wchar_t**>(pField) = pHeap;
    nSize = nLength;
    nCapacity = nLength;
}

// Runs the stock constructor with the MAGMA page name swapped out, so the clone binds to a document
// of its own rather than sharing the one the real page uses.
//
// The name reaches the constructor as a push of a string literal, so the swap is a matter of
// repointing those immediates for the duration of the call. Only pushes whose target really is the
// stock name get touched, which makes a wrong match impossible rather than merely unlikely.
static bool ConstructWithOwnLayout(void* pPage)
{
    uint32_t* pSites[8]{};
    uint32_t nOriginals[8]{};
    size_t nSites = 0;

    auto pCode = reinterpret_cast<uint8_t*>(PageConstruct);
    for (ptrdiff_t i = 0; i < nCtorScanBytes && nSites < std::size(pSites); i++)
    {
        if (pCode[i] != 0x68) // push imm32
            continue;

        auto pImmediate = reinterpret_cast<uint32_t*>(pCode + i + 1);
        auto pTarget = reinterpret_cast<const char*>(static_cast<uintptr_t>(*pImmediate));
        if (!PointsIntoDunia(pTarget) || strncmp(pTarget, szStockPageLayout, sizeof(szStockPageLayout)) != 0)
            continue;

        pSites[nSites] = pImmediate;
        nOriginals[nSites] = *pImmediate;
        nSites++;
    }

    if (nSites == 0)
    {
        JFLog("page: FAILED - no \"%s\" push found in the constructor", szStockPageLayout);
        return false;
    }

    for (size_t i = 0; i < nSites; i++)
        injector::WriteMemory<uint32_t>(pSites[i], reinterpret_cast<uint32_t>(szJackalFixPageLayout), true);

    PageConstruct(pPage);

    for (size_t i = 0; i < nSites; i++)
        injector::WriteMemory<uint32_t>(pSites[i], nOriginals[i], true);

    JFLog("page: constructed against \"%s\" (%zu push site(s) swapped)", szJackalFixPageLayout, nSites);
    return true;
}

// The Game row's delegate on the OPTIONS list carries CFCXOptionGamePage's class id, which is also
// what its page creator pushes - so one immediate out of BuildEntries identifies both the class we
// clone and the creator that tells us how to build it.
// Finding the creator only needs the *address* of the class id, which is a link-time constant, so
// this can run at start-up. Deriving our class record from the engine's cannot: that record is filled
// in lazily by the creator itself, and at plugin init it is still all zeros.
static bool ResolveCreator()
{
    if (PageConstruct != nullptr)
        return true;

    auto pClassId = *reinterpret_cast<uint32_t**>(pBuildEntries + nBuildGameClassId);
    pClonedClassId = pClassId;

    for (size_t i = 0; i < PageCreators.size(); i++)
    {
        auto pCreator = PageCreators.get(i).get<uint8_t>();
        if (*reinterpret_cast<uint32_t**>(pCreator + nCreateClassId) != pClassId)
            continue;

        pPageCreator  = pCreator;
        nPageSize     = *reinterpret_cast<uint32_t*>(pCreator + nCreatePageSize);
        GameAlloc     = reinterpret_cast<GameAlloc_t>(CallTarget(pCreator + nCreateAlloc));
        PageConstruct = reinterpret_cast<PageConstruct_t>(CallTarget(pCreator + nCreateConstruct));
        JFLog("resolve: creator %p size %u alloc %p ctor %p", pCreator, nPageSize, GameAlloc, PageConstruct);
        break;
    }

    if (PageConstruct == nullptr)
    {
        JFLog("resolve: FAILED - none of the %zu creators pushes class id %p", PageCreators.size(), pClassId);
        return false;
    }

    JFLog("resolve: creator hooked for class id %p", pClassId);
    return true;
}

// Runs once the engine has filled the record in, which the creator does on its way past. Everything
// here needs the real contents, so none of it can happen at start-up.
static bool ResolveClassRecord()
{
    if (pJackalFixClassId != nullptr)
        return true;

    auto pRecord = FindClassRecord(pClonedClassId);
    if (pRecord == nullptr)
    {
        JFLog("resolve: FAILED - no class record behind class id %p", pClonedClassId);
        return false;
    }

    JFLog("resolve: cloning \"%s\" (depth %u)", reinterpret_cast<const char*>(pRecord[0]), pRecord[1]);

    // Same shape as the cloned class's record, with one more link on the chain for us. The engine
    // would put a hash of the class name in that last slot; anything unique does just as well, since
    // only the registry reads it. The address of our own name string is unique by construction and
    // can never be the 0xFFFFFFFF that marks a row as having no page behind it.
    JackalFixClassRecord[0] = reinterpret_cast<uint32_t>(szJackalFixPageClass);
    JackalFixClassRecord[1] = pRecord[1] + 1;
    for (uint32_t i = 0; i < pRecord[1]; i++)
        JackalFixClassRecord[2 + i] = pRecord[2 + i];
    JackalFixClassRecord[pRecord[1] + 2] = reinterpret_cast<uint32_t>(szJackalFixPageClass);

    pJackalFixClassId = &JackalFixClassRecord[JackalFixClassRecord[1] + 1];
    JFLog("resolve: ok, our class id is %p", pJackalFixClassId);
    return true;
}

// True when the manager already has our page. The registry is rebuilt along with the menu, so this
// is asked every time rather than cached - a page registered against a previous manager is gone.
static bool HasJackalFixPage(uint8_t* pManager)
{
    if (pManager == nullptr || pJackalFixClassId == nullptr)
        return false;

    auto pRegistry = pManager + nManagerRegistry;
    void* pIterator = nullptr;
    RegistryFind(pRegistry, nullptr, &pIterator, pJackalFixClassId);
    return pIterator != *reinterpret_cast<void**>(pRegistry + nRegistryEnd);
}

static bool CreateJackalFixPage(uint8_t* pManager, void* pParent)
{
    if (!ResolveClassRecord() || pManager == nullptr)
        return false;

    auto pRegistry = pManager + nManagerRegistry;
    if (HasJackalFixPage(pManager))
        return true;

    auto pPage = static_cast<uint8_t*>(GameAlloc(nPageSize, 0));
    if (pPage == nullptr)
    {
        JFLog("page: FAILED - allocation of %u bytes returned null", nPageSize);
        return false;
    }

    if (!ConstructWithOwnLayout(pPage))
        return false;

    if (JackalFixVTable[1] == 0)
    {
        // Copied from one slot before the table so the RTTI pointer that sits there comes with it.
        auto pSource = *reinterpret_cast<uintptr_t**>(pPage) - 1;
        for (size_t i = 0; i <= nVTableSlots; i++)
            JackalFixVTable[i] = pSource[i];

        // The stock open is three statements - build the rows, clear a field, tail call the base -
        // so ours needs the field's offset and the base entry point out of it. Both are read rather
        // than assumed, and a shape that does not match aborts instead of guessing.
        auto pOpen = reinterpret_cast<uint8_t*>(JackalFixVTable[1 + nOpenSlot]);
        if (pOpen[nOpenBuildRows] != 0xE8 || pOpen[nOpenBaseTailJump] != 0xE9)
        {
            JFLog("page: FAILED - the open method at %p is not the expected shape", pOpen);
            return false;
        }

        nPageResetField = *reinterpret_cast<int32_t*>(pOpen + nOpenResetFieldDisp);
        BasePageOpen = reinterpret_cast<PageMethod_t>(RelativeTarget(pOpen + nOpenBaseTailJump, 1));

        // The builder we are replacing is the call this method opens with. We never run it, but its
        // prologue is where the event handler wiring is spelled out.
        auto pStockBuild = CallTarget(pOpen + nOpenBuildRows);
        nHandlerSize     = *reinterpret_cast<uint8_t*>(pStockBuild + nBuildHandlerSize);
        nEventDispatcher = *reinterpret_cast<int32_t*>(pStockBuild + nBuildDispatcherDisp);
        HandlerInit      = reinterpret_cast<HandlerInit_t>(CallTarget(pStockBuild + nBuildHandlerInit));
        DispatcherSlot   = reinterpret_cast<DispatcherSlot_t>(CallTarget(pStockBuild + nBuildDispatcherSlot));
        RegisterHandler  = reinterpret_cast<RegisterHandler_t>(CallTarget(pStockBuild + nBuildRegisterHandler));
        for (size_t i = 0; i < std::size(HandlerVTables); i++)
            HandlerVTables[i] = *reinterpret_cast<uint32_t*>(pStockBuild + nBuildHandlerVTables[i]);

        JFLog("page: builder %p dispatcher +%X handler size %u init %p slot %p register %p",
            pStockBuild, nEventDispatcher, nHandlerSize, HandlerInit, DispatcherSlot, RegisterHandler);

        JackalFixVTable[1 + nOpenSlot] = reinterpret_cast<uintptr_t>(JackalFixOpenPage);
        JackalFixVTable[1 + nClassRecordSlot] = reinterpret_cast<uintptr_t>(JackalFixGetClassRecord);

        JFLog("page: open %p, base open %p, reset field +%X", pOpen, BasePageOpen, nPageResetField);
    }

    *reinterpret_cast<uintptr_t**>(pPage) = &JackalFixVTable[1];

    *reinterpret_cast<void**>(pPage + nPageManager) = pManager;
    *reinterpret_cast<void**>(pPage + nPageParent) = pParent;
    SetPageTitle(pPage, szJackalFixPageTitle);

    auto ppSlot = RegistryInsert(pRegistry, nullptr, pJackalFixClassId);
    if (ppSlot == nullptr)
    {
        JFLog("page: FAILED - registry insert returned null");
        return false;
    }

    *ppSlot = pPage;
    pJackalFixPage = pPage;
    JFLog("page: created %p, registered in %p", pPage, pRegistry);
    return true;
}

// ------------------------------------------------------------------------------------------------

static SafetyHookInline BuildEntriesHook{};
static SafetyHookInline PageCreatorHook{};

// Our page is built in the same pass the engine builds its own, rather than later on demand. That
// ordering is not cosmetic: a page registered after the manager's start-up pass never gets its
// document loaded or its ready flag set, and opening it walks null. Creating it here, alongside the
// game options page and off the same manager and parent, means it is initialised like any other.
static void* __fastcall CreateGamePage(void* pManager, void* pEdx, void* pParent)
{
    auto pGamePage = PageCreatorHook.fastcall<void*>(pManager, pEdx, pParent);

    JFLog("create: manager %p parent %p game page %p", pManager, pParent, pGamePage);
    CreateJackalFixPage(static_cast<uint8_t*>(pManager), pParent);

    return pGamePage;
}

static void __fastcall BuildEntries(void* pPage, void* pEdx)
{
    // After the original, so our row lands below Controls.
    BuildEntriesHook.fastcall(pPage, pEdx);

    // The row goes in either way. If the page is missing it is added greyed out rather than skipped,
    // so "no row at all" and "row that does nothing" stay distinguishable symptoms.
    auto pManager = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pPage) + nPageManager);

    void* pDelegate = nullptr;
    if (HasJackalFixPage(pManager))
    {
        if (auto pStorage = GameAlloc(nDelegateSize, 0))
            pDelegate = MakeDelegate(pStorage, nullptr, pPage, pJackalFixClassId, 0, 1);
    }
    else
    {
        JFLog("build: no page registered against manager %p", pManager);
    }

    AddEntry(pPage, nullptr, L"Jackal Fix Options", pDelegate != nullptr, pDelegate);
}

class JackalFixMenu
{
public:
    JackalFixMenu()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            JFLog("init: Dunia at %p", hDunia);

            // CFCXOptionPage::BuildEntries. Entry through the first sub page's delegate allocation;
            // the 7Ch size and the guard test that follows it are unique to this function.
            auto buildPattern = dunia_pattern("83 EC 40 53 55 56 57 33 DB 53 6A 7C 8B F1 E8 ? ? ? ? 8B F8 83 C4 08 3B FB 74 23");
            if (buildPattern.empty())
            {
                JFLog("init: FAILED - the BuildEntries pattern matched nothing");
                return;
            }

            // CListMenuPage::AddValueRow, the two-value row used by every options page. Anchored on
            // the 5Ch control allocation that follows the row append.
            auto rowPattern = dunia_pattern("51 8B 44 24 20 8B 54 24 1C 53 55 56 57 8B 7C 24 18 50 52 57 89 4C 24 1C E8 ? ? ? ? 33 DB 8B E8 53 6A 5C");
            if (rowPattern.empty())
            {
                JFLog("init: FAILED - the AddValueRow pattern matched nothing");
                return;
            }

            // The page creation template, instantiated once per page class. Every instance shares
            // the same body, so any match resolves the registry helpers; the instance that builds
            // the class we clone is picked out later by the class id it pushes.
            PageCreators = dunia_pattern("51 83 3D ? ? ? ? 00 53 57 8B F9 75 07 33 C9 E8 ? ? ? ? 68 ? ? ? ? 8D 44 24 0C 8D 5F 04 50 8B CB E8 ? ? ? ? "
                                         "8B 44 24 08 3B 47 14 75 ? 56 6A 00 68 ? ? ? ? E8 ? ? ? ? 83 C4 08 85 C0 74 0B 8B C8 E8 ? ? ? ? 8B F0 EB 02 33 F6 "
                                         "8B 4C 24 14 89 BE 40 01 00 00 89 8E EC 00 00 00 83 7F 18 00 75 03 89 77 38 83 3D ? ? ? ? 00 75 07 33 C9 E8 ? ? ? ? "
                                         "68 ? ? ? ? 8B CB E8 ? ? ? ? 89 30");
            if (PageCreators.empty())
            {
                JFLog("init: FAILED - the page creator pattern matched nothing");
                return;
            }

            pBuildEntries = buildPattern.get_first<uint8_t>();
            AddValueRow   = reinterpret_cast<AddValueRow_t>(rowPattern.get_first<uint8_t>());

            nDelegateSize = *reinterpret_cast<uint8_t*>(pBuildEntries + nBuildDelegateSize);
            MakeDelegate  = reinterpret_cast<MakeDelegate_t>(CallTarget(pBuildEntries + nBuildMakeDelegate));
            AddEntry      = reinterpret_cast<AddEntry_t>(CallTarget(pBuildEntries + nBuildAddEntry));

            // Any instance will do here; the two registry helpers are shared by all of them.
            auto pCreator  = PageCreators.get(0).get<uint8_t>();
            RegistryFind   = reinterpret_cast<RegistryFind_t>(CallTarget(pCreator + nCreateRegistryFind));
            RegistryInsert = reinterpret_cast<RegistryInsert_t>(CallTarget(pCreator + nCreateRegistryInsert));

            JFLog("init: BuildEntries %p, AddValueRow %p, %zu creators, delegate size %u", pBuildEntries, AddValueRow, PageCreators.size(), nDelegateSize);
            JFLog("init: MakeDelegate %p AddEntry %p RegistryFind %p RegistryInsert %p", MakeDelegate, AddEntry, RegistryFind, RegistryInsert);

            BuildEntriesHook = safetyhook::create_inline(pBuildEntries, BuildEntries);
            JFLog("init: BuildEntries hook %s", BuildEntriesHook.enabled() ? "installed" : "FAILED TO INSTALL");

            // Only the creator is resolved here. Its class record is still empty at this point -
            // the creator fills it in the first time it runs - so our own record waits until then.
            if (!ResolveCreator())
                return;

            PageCreatorHook = safetyhook::create_inline(pPageCreator, CreateGamePage);
            JFLog("init: page creator %p hook %s", pPageCreator, PageCreatorHook.enabled() ? "installed" : "FAILED TO INSTALL");
        };
    }
} JackalFixMenu;
