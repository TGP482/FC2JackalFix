module;

#include <common.hxx>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>

export module jackalfixmenu;

import common;
import dunia;
import settings;
import borderless;    // the game window, for measuring what a render resolution percentage is of
import renderconfig;  // which of the game's own quality presets are in force

// The options menu is not data driven. options.mgb carries the page artwork and the navbar prompts;
// the rows under OPTIONS are built in code. CFCXOptionPage::BuildEntries appends one row per sub
// page (Game, Display, Sound, Network, Controls) and hands each row a delegate holding the target
// page's class id. Activating a row looks that id up in the menu manager's page registry and makes
// whatever it finds the next page.
//
// So a new page is a sixth append plus a page object registered under a class id of our own. No
// .mgb editing, no hex editor, and the row inherits the engine's own highlight, sound and
// transition behaviour because it is an ordinary row.
//
// What the page can hold is a separate question from what it can show. The label list is a
// magma::ListBox and has no row ceiling at all: its items are an unbounded vector and a row's Y is
// computed as lineAreaTop + index * (lineHeight + spacing) rather than being furniture. The ceiling
// is on the value side: each value row binds by name to a widget the layout already declares, and
// MAINMENU_OPTIONGAME_PAGE declares eight of them. That is why the whole settings list is held as a
// virtual list here and only a window of it is ever handed to the engine.

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

// magma::ListBox, the widget behind the page's row list. Only the fields this file touches.
//
// nListBoxWrap is the single flag behind two behaviours that read as separate ones: it decides
// whether the selection wraps at the ends, and whether the little edge arrows grey out there. With
// it clear, pressing down on the last row leaves the selection untouched and the nav handler
// reports the refusal, which is exactly the signal a paged list needs, so it is deliberately
// cleared.
static constexpr ptrdiff_t nListBoxMaxVisible   = 0x18; // byte; visible line count is min(this, item count)
static constexpr ptrdiff_t nListBoxFlags        = 0x19; // bit 0 = wrap at the ends
static constexpr uint8_t   nListBoxWrap         = 0x01;
static constexpr ptrdiff_t nListBoxItemsBegin   = 0x94;
static constexpr ptrdiff_t nListBoxItemsEnd     = 0x98;
static constexpr ptrdiff_t nListBoxVisibleCount = 0xC8;
static constexpr ptrdiff_t nListBoxFirstVisible = 0xCC;
// The item vector, and the two fields on an item that decide these two separate things.
//
// +44h is the disabled flag. It is the engine's own answer to "the selection may not rest here" -
// SetSelection refuses such a row outright, so no path that lights a row can reach it. That is why
// adding headings disabled worked, and it is the only thing that ever has.
//
// +38h is the cached visual state, and it is what made disabling them unacceptable: it is computed
// from the same flag, so a disabled row is also drawn in the greyed ink. They are separate fields
// though, so the flag can be set for the selection's benefit and the cached state put back to what
// an ordinary row uses.
static constexpr ptrdiff_t nListBoxItems       = 0x94;
static constexpr size_t    nItemSize           = 0x54;
static constexpr ptrdiff_t nItemVisualState    = 0x38;
static constexpr ptrdiff_t nItemDisabled       = 0x44;

static constexpr ptrdiff_t nListBoxHighlight    = 0xD0;
static constexpr ptrdiff_t nListBoxSelected     = 0xD4;

// The nav handler is dispatched from the widget's own table, so overriding it is a per-instance
// vtable swap rather than a hook, which matters because every list in the game shares the class and
// only this page's list should behave differently.
static constexpr size_t nListBoxNavSlot    = 27; // +6Ch
static constexpr size_t nListBoxHoverSlot  = 28; // +70h, where the pointer moving over a row lands
static constexpr size_t nListBoxMouseSlot  = 29; // +74h, where a click lands
static constexpr size_t nListBoxPointerSlot = 30; // +78h, the other pointer entry

// Those four are not a guess: the function that lights a row is called from exactly slots 27, 28,
// 29 and 30 of this table and nowhere else, so those four are the complete set of ways a row can be
// lit, and all four have to agree that a section title is not a row.
static constexpr size_t nListBoxVTableSlots = 45;

// What the nav handler writes into its result when it could not move the selection: a direction
// code and a flag asking the focus manager to hand the input to whatever is next. Swallowing that
// flag is what stops a page turn from also moving focus off the list.
static constexpr ptrdiff_t nNavResultCode   = 0x10;
static constexpr ptrdiff_t nNavResultFlags  = 0x16;
static constexpr uint8_t   nNavFlagUnhandled = 0x04;
static constexpr int       nNavCodeUp        = 0;
static constexpr int       nNavCodeDown      = 1;

// Page vtable slots. One and two are the same on every page class in this engine; sixteen is the
// row-clearing entry the stock builders call before repopulating.
//
// Nineteen, twenty and twenty-one are the three slots a clone must own. Twenty and twenty-one are
// the option-page layer's Apply and Revert: the action buttons are handled by generic code on that
// layer (0x1087EB10, 0x1087F090, 0x1087F1F0) which dispatches straight back through them. Nineteen
// is the page's own value-changed hook, which the engine raises on every single arrow press -
// CFCXOptionGamePage uses it to show or hide the machete row when difficulty moves.
//
// All three read the row indices CFCXOptionGamePage stashes at page+1D8h..1F8h. On a clone every
// one of those is still -1, and the map lookup they feed is a std::map::operator[], so it inserts a
// null against key -1 and hands back a pointer to it. What follows is this engine's habit of
// writing a null check that is not a guard:
//
//     control = *map[page->difficultyRow];
//     if (control == 0) control = 0;          // "handled"
//     (**(code**)(*control + 0x38))();        // 1081F70Ch, dereferenced regardless
//
// Owning these three slots is what makes the stock action handlers safe to reuse, and it is why
// Apply, Revert and Back can behave like a real options page instead of being stubbed out.
static constexpr size_t nClassRecordSlot  = 1;
static constexpr size_t nOpenSlot         = 2;
static constexpr size_t nClearRowsSlot    = 16;
static constexpr size_t nValueChangedSlot = 19; // +4Ch
static constexpr size_t nApplySlot        = 20; // +50h
static constexpr size_t nRevertSlot       = 21; // +54h

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

// The stock row builder opens by registering three handlers against the page's own action
// dispatcher. They are Apply, Revert and Back rather than, as first assumed, previous/next/
// activated value. Replacing that builder wholesale meant our page had none, and the input path
// dispatches into a null object the moment a prompt is pressed. Everything needed to register them
// is read out of the builder's prologue.
static constexpr ptrdiff_t nBuildHandlerSize     = 0x0E; // push 0Ch, the handler allocation size
static constexpr ptrdiff_t nBuildHandlerInit     = 0x22; // call <handler base ctor>
static constexpr ptrdiff_t nBuildDispatcherDisp  = 0x37; // disp32 of lea ebp,[esi+disp32]
static constexpr ptrdiff_t nBuildDispatcherSlot  = 0x3F; // call <slot for event id>
static constexpr ptrdiff_t nBuildRegisterHandler = 0x46; // call <register>
static constexpr ptrdiff_t nBuildHandlerVTables[]{ 0x29, 0x66, 0x9D }; // mov [edi], <vtable>

// The base open's prologue is where the page's own bookkeeping for the action prompts is spelled
// out, and it is the only place all three pieces appear together:
//
//     mov  byte ptr [edi+1B8h], 0     ; nothing has been changed yet
//     push 0 / push 1 / call <slot>   ; prompt one is APPLY
//     call <set enabled>              ; ...and it starts greyed
//     push 1 / push 2 / call <slot>   ; prompt two is DEFAULT
//     call <set enabled>              ; ...and it starts lit
//
// The byte at 1B8h is the whole mechanism: the per-row change handler lights APPLY only while it
// is clear, the APPLY handler refuses to do anything while it is clear, and the confirmation box
// behind DEFAULT clears it again afterwards. Rebuilding our rows runs through the same base open,
// so all three have to be read out rather than assumed.
static constexpr ptrdiff_t nBaseOpenDirtyOpcode = 0x1B; // mov byte ptr [edi+disp32], 0
static constexpr ptrdiff_t nBaseOpenDirtyDisp   = 0x1D;
static constexpr ptrdiff_t nBaseOpenPromptSlot  = 0x22; // call <slot for event id>
static constexpr ptrdiff_t nBaseOpenSetEnabled  = 0x29; // call <light or grey that prompt>

// Which prompt is which, in the numbering the dispatcher uses.
static constexpr int nPromptApply   = 1;
static constexpr int nPromptDefault = 2;

// ------------------------------------------------------------------------------------------------
// Engine functions. The __thiscall ones are declared __fastcall with an unused EDX, which is the
// same calling convention with the hidden argument spelled out.

using GameAlloc_t      = void*    (__cdecl*)(size_t nSize, int nUnused);
using PageConstruct_t  = void*    (__fastcall*)(void* pPage);
using PageMethod_t     = void     (__fastcall*)(void* pPage, void* pEdx);
using RegistryFind_t   = void     (__fastcall*)(void* pRegistry, void* pEdx, void** ppIterator, const uint32_t* pClassId);
using RegistryInsert_t = void**   (__fastcall*)(void* pRegistry, void* pEdx, const uint32_t* pClassId);
using MakeDelegate_t   = void*    (__fastcall*)(void* pDelegate, void* pEdx, void* pOwnerPage, const uint32_t* pClassId, int nUnused, char bSetParent);
using AddEntry_t       = int      (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pText, int bEnabled, void* pDelegate);

// Appends a row carrying a two-value control, the < Yes / No > arrows used across the options
// pages. Internally it is AddEntry plus a control stamped from pTemplate into the page's own
// document, then registered in a map at page+190h. That map is why this only works on pages derived
// from the value-page base: a plain CListMenuPage is 190h bytes and has no such member.
using AddValueRow_t    = void*    (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pLabel, const char* pTemplate,
                                                const char* pWidgetName, const wchar_t* pOnText, const wchar_t* pOffText,
                                                int bEnabled, void* pDelegate);

// The same idea with an integer range instead of two texts. The control it builds is a different
// class, a CSliderSetting of 50h bytes against the two-value row's 5Ch, and it keeps its widget at
// control+48h rather than +44h, which is why the null test below is written per row kind. Min and
// max really are ints: the bind call sets the widget's integer-mode flag, so it steps by one.
using AddSliderRow_t   = void*    (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pLabel, const char* pTemplate,
                                                const char* pWidgetName, int nMinimum, int nMaximum,
                                                int bEnabled, void* pDelegate);

// A list of arbitrary choices. The count comes before both arrays, which is the easy thing to get
// wrong. Both arrays are copied into the control as it is built, so neither has to outlive the
// call: the labels are deep copied into the widget's own item list and the values into an int array
// at control+4Ch. This still draws as arrows, being a longer value list rather than a popup.
using AddMultiValueRow_t = void*  (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pLabel, const char* pTemplate,
                                                const char* pWidgetName, uint32_t nCount,
                                                const wchar_t* const* pLabels, const int* pValues,
                                                int bEnabled, void* pDelegate);

// Selects the value whose byte matches, by searching the control's own value list.
using SetRowValue_t    = void     (__fastcall*)(void* pControl, void* pEdx, const void* pValue);
using GetRowValue_t    = const void* (__fastcall*)(void* pControl, void* pEdx);
static constexpr size_t nRowSetValueSlot = 13;
static constexpr size_t nRowGetValueSlot = 14;

// magma::ListBox's input handler, taken over per instance. The result argument is where it records
// that it could not act on the key.
using ListBoxNav_t = int (__fastcall*)(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult);

static GameAlloc_t        GameAlloc        = nullptr;
static PageConstruct_t    PageConstruct    = nullptr;
static RegistryFind_t     RegistryFind     = nullptr;
static RegistryInsert_t   RegistryInsert   = nullptr;
static MakeDelegate_t     MakeDelegate     = nullptr;
static AddEntry_t         AddEntry         = nullptr;
static AddValueRow_t      AddValueRow      = nullptr;
static AddSliderRow_t     AddSliderRow     = nullptr;
static AddMultiValueRow_t AddMultiValueRow = nullptr;
static PageMethod_t       BasePageOpen     = nullptr;

// The stock call site pushes the handler, then the event id, then calls both functions in turn,
// each taking a single stack argument, so the slot call consumes the event id and leaves the
// handler in place for the register call. Reading it as one call with two arguments unbalances the
// stack by eight bytes, which is what the runtime check catches.
using HandlerInit_t     = void* (__fastcall*)(void* pHandler, void* pEdx, int nUnused);
using DispatcherSlot_t  = void* (__fastcall*)(void* pDispatcher, void* pEdx, int nEvent);
using RegisterHandler_t = void  (__fastcall*)(void* pSlot, void* pEdx, void* pHandler);

// Lights or greys the prompt behind one of those slots.
using SetPromptEnabled_t = void (__fastcall*)(void* pSlot, void* pEdx, int bEnabled);

static HandlerInit_t     HandlerInit     = nullptr;
static DispatcherSlot_t  DispatcherSlot  = nullptr;
static RegisterHandler_t RegisterHandler = nullptr;
static SetPromptEnabled_t SetPromptEnabled = nullptr;
static uint32_t          HandlerVTables[3]{};
static uint32_t          nHandlerSize    = 0;
static ptrdiff_t         nEventDispatcher = 0;
static ptrdiff_t         nPageChanged    = 0;

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
// The page has to derive from the value-page base or the row APIs have nowhere to register their
// controls, so the clone is a CFCXOptionGamePage rather than the plain CFCXOptionPage the OPTIONS
// list itself uses.
//
// A page's widgets are also not private to it: CListMenuPage hashes the name of its MAGMA page and
// resolves the document from that hash, so two page objects naming the same MAGMA page share one
// document, one list widget and one selected index. So the clone names a MAGMA page of its own.
// MAINMENU_OPTIONGAME_PAGE is the console cut of game options: it ships in the PC options.mgb,
// whose options.mgb.desc carries a navbar block for it, but the PC build only ever names
// MAINMENU_OPTIONGAME_PAGE_PC, and the plain name appears nowhere in Dunia's strings.
static const char szStockPageLayout[]     = "MAINMENU_OPTIONGAME_PAGE_PC";
static const char szJackalFixPageLayout[] = "MAINMENU_OPTIONGAME_PAGE";

static const wchar_t szJackalFixPageTitle[] = L"JACKAL FIX";

// Widget template each value row is stamped from.
static const char szRowTemplate[] = "SETTING_LABEL_LIST";

// A value row binds to a widget that already exists in the layout, by name. The name is not ours to
// invent: the bind call looks it up, stores the result on the control, and silently skips adding
// the choices when the lookup fails, which is exactly why the arrows once drew with no Yes or No
// against them.
static constexpr ptrdiff_t nControlNamedWidget = 0x44; // two-value / multi-value: widget found by name
static constexpr ptrdiff_t nSliderNamedWidget  = 0x48; // CSliderSetting keeps it one dword further on
static constexpr ptrdiff_t nControlRowIndex    = 0x40; // this row's index into the page's row list

// How far into the constructor to look for the layout name. Generous and bounded.
static constexpr ptrdiff_t nCtorScanBytes = 0x600;

// ------------------------------------------------------------------------------------------------
// The layout's lines.
//
// Labels are dynamic rows in the page's nav list. Value widgets are fixed furniture in the MAGMA
// layout, each drawn on the line the layout assigns it, and they line up only when row N is paired
// with the widget sitting on line N.
//
// Decoding options.mgb gives MAINMENU_OPTIONGAME_PAGE a settings map of ten entries, each binding a
// SETTING_* name to a path ending in a row container p_setting_1..8, and those containers carry
// their own Y coordinates: 225, 250, 278, 306, 335, 364, 391 and 420. Sorting by Y gives the order
// below, and each container's child link says whether the line holds arrows (p_list) or a slider.
//
// Two names in that map are dead and must not be used: SETTING_9 is present but its link has a
// component count of zero, and SETTING_10 is not in the map at all. Either would fail the name
// lookup, leave the control's widget pointer null and draw a blank row.
//
// The slider line is the awkward one. There is exactly one of it per eight, so a page holding two
// slider rows is impossible until the layout carries more, and a list that mixes kinds can only be
// scrolled a whole window at a time, because a line's kind is a property of its position. So
// numeric settings are declared as ranges rather than sliders (see ROW_RANGE) and drawn with
// arrows, which keeps every usable line the same kind and every page densely packed. The slider
// line is left to draw faded until the layout grows.
enum WidgetKind
{
    WIDGET_ARROWS,
    WIDGET_SLIDER,
    WIDGET_UNUSABLE, // a line whose widget no row can currently be paired with
};

struct PageLine
{
    const char* pWidgetName;
    WidgetKind  nKind;
};

// The eight the layout ships with. SETTING_9 and SETTING_10 are in the settings map but dead:
// SETTING_9's link has a component count of zero and SETTING_10 is absent, so neither is listed.
static const PageLine StockLines[]
{
    { "SETTING_1",           WIDGET_ARROWS   }, // p_setting_1, y 225
    { "SETTING_SENSITIVITY", WIDGET_UNUSABLE }, // p_setting_2, y 250 - the lone slider line
    { "SETTING_3",           WIDGET_ARROWS   }, // p_setting_3, y 278
    { "SETTING_4",           WIDGET_ARROWS   }, // p_setting_4, y 306
    { "SETTING_5",           WIDGET_ARROWS   }, // p_setting_5, y 335
    { "SETTING_6",           WIDGET_ARROWS   }, // p_setting_6, y 364
    { "SETTING_7",           WIDGET_ARROWS   }, // p_setting_7, y 391
    { "SETTING_8",           WIDGET_ARROWS   }, // p_setting_8, y 420
};

// ------------------------------------------------------------------------------------------------
// How the page is laid out, and the two numbers worth tuning.
//
// nLiftUnits is how far the title and the whole row block move up, in MAGMA layout units. The
// package resolves at 1280x800, so a unit is 800ths of the screen height regardless of the actual
// resolution. It is measured to put the title roughly where ACTION sits on the controls page.
//
// It is deliberately a constant rather than anything cleverer: the drawn position of a row is its
// container coordinate plus whatever its parents contribute, so the only honest way to land it
// exactly is to look at it. This is the number to do that with.
static constexpr int    nLiftUnits   = 94;

// Room for the stock eight, with headroom for the day the layout carries more.
static constexpr size_t nMaxLines = 24;

static PageLine RowLines[nMaxLines]{};
static size_t   nRowLines = 0;

// The widget each line draws its value in, and the y it belongs at. Kept because a line whose row
// carries no value, a heading or a blank, has to have its arrows taken off the screen rather than
// left drawing against nothing, and they have to come back when a value row next lands there.
static void*   LineWidgets[nMaxLines]{};
static int16_t LineY[nMaxLines]{};

// The widget name each line binds to, held somewhere with a lifetime, because the names of the rows
// added to the layout are made up at run time rather than written down.
static char LineNames[nMaxLines][24]{};

// Somewhere off the bottom of the page. A widget parked here is one nobody can see; nothing else
// about it changes, which is why this is preferred over reaching for a visibility flag.
static constexpr int16_t nParkedY = 4000;

// Worth recording, because it is not obvious and it caught me out: a row's value widget is the
// innermost list in a chain (options / p_option_game / p_setting_N / p_list / l_setting) and its
// own coordinate is zero. The per-row offset lives on the container above it, which is not what a
// name resolves to, so all eight rows read as y 0 and the lift is applied to that rather than to a
// measured position.

// ------------------------------------------------------------------------------------------------
// The page's contents, declared per ini section and in ini order.

enum RowKind
{
    ROW_HEADING, // a label with no control; consumes a line, and that line's widget draws faded
    ROW_BOOL,    // two-value arrows reading Yes / No
    ROW_TICK,    // the same control, reading [X] / [  ]
    ROW_ENUM,    // a named list of choices
    ROW_RANGE,   // a numeric range, drawn as a generated list of steps
    ROW_SLIDER,  // a true slider; needs a WIDGET_SLIDER line
};

enum ValueKind
{
    VALUE_INT,
    VALUE_FLOAT,
};

struct MenuRow
{
    RowKind        nKind;
    const wchar_t* pLabel;

    Pref           nPref;
    const char*    pIniSection;
    const char*    pIniKey;
    ValueKind      nValue;

    // ROW_RANGE and ROW_SLIDER. The control is integral, so a float setting is carried in fixed
    // point: the row works in units of the ini value multiplied by fScale, and converts at the
    // edges. Minimum, maximum and step are all in those units.
    int   nMinimum;
    int   nMaximum;
    int   nStep;
    float fScale;
    const wchar_t* pFormat; // how a step is written out, e.g. L"%d" or L"%.2f"

    // ROW_ENUM. Both arrays are copied by the engine as the row is built, so static storage is
    // fine.
    const int*            pValues;
    const wchar_t* const* pValueLabels;
    uint32_t              nValueCount;

    // A second setting moved in step with the first, for the cases where one row stands for a
    // pair: an internal resolution is one choice and two ini keys. Its value comes from the
    // matching entry of pValues2.
    Pref        nPref2;
    const char* pIniKey2;
    const int*  pValues2;
};

static constexpr MenuRow Heading(const wchar_t* pLabel)
{
    return { ROW_HEADING, pLabel, Pref::COUNT, nullptr, nullptr, VALUE_INT,
             0, 0, 1, 1.0f, nullptr, nullptr, nullptr, 0, Pref::COUNT, nullptr, nullptr };
}

static constexpr MenuRow Boolean(const wchar_t* pLabel, Pref nPref, const char* pSection, const char* pKey)
{
    return { ROW_BOOL, pLabel, nPref, pSection, pKey, VALUE_INT,
             0, 1, 1, 1.0f, nullptr, nullptr, nullptr, 0, Pref::COUNT, nullptr, nullptr };
}

// The same control as Boolean, wearing a tick. There is no checkbox in this menu: the value-page
// base has no API that makes one and MAINMENU_OPTIONGAME_PAGE has no Checkable widget to bind to,
// so the ticks on the display page are out of reach. What a two-value row can choose is what its
// two states read as, which is close enough to be worth having.
static constexpr MenuRow Tick(const wchar_t* pLabel, Pref nPref, const char* pSection, const char* pKey)
{
    return { ROW_TICK, pLabel, nPref, pSection, pKey, VALUE_INT,
             0, 1, 1, 1.0f, nullptr, nullptr, nullptr, 0, Pref::COUNT, nullptr, nullptr };
}

// Boolean over a setting the ini carries as a float, so the two states write 0 and 1 rather than
// an integer the reader would not recognise.
static constexpr MenuRow BooleanFloat(const wchar_t* pLabel, Pref nPref, const char* pSection, const char* pKey)
{
    return { ROW_BOOL, pLabel, nPref, pSection, pKey, VALUE_FLOAT,
             0, 1, 1, 1.0f, nullptr, nullptr, nullptr, 0, Pref::COUNT, nullptr, nullptr };
}

template<size_t N>
static constexpr MenuRow Enumeration(const wchar_t* pLabel, Pref nPref, const char* pSection, const char* pKey,
                                     const int (&Values)[N], const wchar_t* const (&Labels)[N])
{
    return { ROW_ENUM, pLabel, nPref, pSection, pKey, VALUE_INT,
             0, 0, 1, 1.0f, nullptr, Values, Labels, N, Pref::COUNT, nullptr, nullptr };
}

// One row, two ini keys. The chosen index selects from both arrays at once.
template<size_t N>
static constexpr MenuRow EnumerationPair(const wchar_t* pLabel, Pref nPref, const char* pSection, const char* pKey,
                                         Pref nPref2, const char* pKey2,
                                         const int (&Values)[N], const int (&Values2)[N],
                                         const wchar_t* const (&Labels)[N])
{
    return { ROW_ENUM, pLabel, nPref, pSection, pKey, VALUE_INT,
             0, 0, 1, 1.0f, nullptr, Values, Labels, N, nPref2, pKey2, Values2 };
}

static constexpr MenuRow Range(const wchar_t* pLabel, Pref nPref, const char* pSection, const char* pKey,
                               ValueKind nValue, int nMinimum, int nMaximum, int nStep,
                               float fScale = 1.0f, const wchar_t* pFormat = L"%d")
{
    return { ROW_RANGE, pLabel, nPref, pSection, pKey, nValue,
             nMinimum, nMaximum, nStep, fScale, pFormat, nullptr, nullptr, 0, Pref::COUNT, nullptr, nullptr };
}

// Declared for completeness and for the day the layout carries more than one slider line. A slider
// row placed on a page with no WIDGET_SLIDER line left is dropped rather than shown wrong.
static constexpr MenuRow Slider(const wchar_t* pLabel, Pref nPref, const char* pSection, const char* pKey,
                                ValueKind nValue, int nMinimum, int nMaximum, float fScale = 1.0f)
{
    return { ROW_SLIDER, pLabel, nPref, pSection, pKey, nValue,
             nMinimum, nMaximum, 1, fScale, L"%d", nullptr, nullptr, 0, Pref::COUNT, nullptr, nullptr };
}

// ------------------------------------------------------------------------------------------------
// Choice lists.

static const int     OffOnValues[]{ 0, 1 };
static const wchar_t* const OffOnLabels[]{ L"Off", L"On" };

static const int     DisplayModeValues[]{ 1, 2, 3 };
static const wchar_t* const DisplayModeLabels[]{ L"Fullscreen", L"Borderless", L"Windowed" };

static const int     ScalingFilterValues[]{ 0, 1 };
static const wchar_t* const ScalingFilterLabels[]{ L"Point", L"Bilinear" };

// Render Resolution is InternalResolutionX/Y and nothing else. The row is a list of percentages of
// whatever that pair is set to, both axes scaled together so the aspect the player asked for is the
// aspect they keep, and each choice writes the two keys as one.
//
// A hundred per cent is the pair itself, which is why there is no "off": the pair unset already
// means the window, and a hundred per cent of the window is the window.
static constexpr int nRenderScaleStep = 10;
static constexpr int nRenderScaleFloor = 20;
static constexpr int nRenderScaleCeiling = 200;

// The Xbox 360 cut, offered as an extra when the base is 16:9. Not a percentage of anything, so it
// sits at the head of the list as a pair of its own.
static constexpr int nConsoleWidth = 1280;
static constexpr int nConsoleHeight = 696;

// The pixel limits the percentages are held inside: the smallest frame the engine copes with, and
// the largest square a D3D9 render target can describe.
static constexpr int nRenderPixelMinW = 320;
static constexpr int nRenderPixelMinH = 240;
static constexpr int nRenderPixelMax = 16384;

// The choices, filled in as the page opens because the pair they are a percentage of is only known
// then. One entry per offered step: the X pixels, the Y pixels, and how it reads.
static constexpr size_t nMaxRenderChoices = 32;
static constexpr size_t nRenderLabelLength = 32;

static int     RenderChoiceX[nMaxRenderChoices]{};
static int     RenderChoiceY[nMaxRenderChoices]{};
static wchar_t RenderChoiceText[nMaxRenderChoices][nRenderLabelLength]{};
static const wchar_t* RenderChoiceLabels[nMaxRenderChoices]{};
static uint32_t nRenderChoices = 0;

static const int     MaxFrameRateValues[]{ 0, 1, 30, 60, 72, 75, 90, 120, 144, 165, 180, 240 };
static const wchar_t* const MaxFrameRateLabels[]{ L"Unlocked", L"Screen Refresh", L"30Hz", L"60Hz", L"72Hz", L"75Hz",
                                                  L"90Hz", L"120Hz", L"144Hz", L"165Hz", L"180Hz", L"240Hz" };

static const int     AnisotropyValues[]{ 0, 2, 4, 8, 16 };
static const wchar_t* const AnisotropyLabels[]{ L"Game default", L"2x", L"4x", L"8x", L"16x" };

static const int     BeyondUltraValues[]{ 0, 1, 2, 3 };
static const wchar_t* const BeyondUltraLabels[]{ L"Default", L"2x", L"4x", L"Max draw distance" };

static const int     BeyondUltraShadowValues[]{ 0, 1 };
static const wchar_t* const BeyondUltraShadowLabels[]{ L"Default", L"Max draw distance" };

static const int     SkipTutorialsValues[]{ 0, 1, 2 };
static const wchar_t* const SkipTutorialsLabels[]{ L"Off", L"Pop-ups", L"Pop-ups and hints" };

// Zero means every CPU, which is the default and wants saying rather than showing as a number.
static const int     CpuAffinityValues[]{ 0, 1, 2, 4, 6, 8, 12, 16 };
static const wchar_t* const CpuAffinityLabels[]{ L"All CPUs", L"1", L"2", L"4", L"6", L"8", L"12", L"16" };

// ------------------------------------------------------------------------------------------------
// The sections, in the order the ini declares them.

// Not const: the render resolution row's ends are settled against the window as the page opens.
static MenuRow DisplayRows[]
{
    Heading(L"DISPLAY"),
    Enumeration(L"Display Mode", PREF_DISPLAYMODE, "Display", "DisplayMode", DisplayModeValues, DisplayModeLabels),
    // Built by hand rather than through EnumerationPair: the choices are filled in as the page
    // opens, so the count is settled there and the helper's would be the array's capacity.
    { ROW_ENUM, L"Render Resolution", PREF_INTERNALRESOLUTIONX, "Display", "InternalResolutionX",
      VALUE_INT, 0, 0, 1, 1.0f, nullptr, RenderChoiceX, RenderChoiceLabels, 0,
      PREF_INTERNALRESOLUTIONY, "InternalResolutionY", RenderChoiceY },
    Enumeration(L"Scaling Filter", PREF_SCALINGFILTER, "Display", "ScalingFilter", ScalingFilterValues, ScalingFilterLabels),
    Enumeration(L"Max Frame Rate", PREF_MAXFRAMERATE, "Display", "MaxFrameRate", MaxFrameRateValues, MaxFrameRateLabels),
};

static const MenuRow GraphicsRows[]
{
    Heading(L"GRAPHICS"),
    Boolean(L"Utilisation", PREF_UTILISATION, "General", "Utilisation"),
    Enumeration(L"Anisotropic Filtering", PREF_ANISOTROPICFILTERING, "Graphics", "AnisotropicFiltering",
                AnisotropyValues, AnisotropyLabels),
    Boolean(L"Xbox 360 Gamma", PREF_X360GAMMA,     "Graphics", "Xbox360Gamma"),
    Boolean(L"No Rim Lighting", PREF_NORIMLIGHTING, "Graphics", "NoRimLighting"),
    Range  (L"Saturation", PREF_SATURATION, "Graphics", "Saturation", VALUE_FLOAT, 0, 100, 5, 100.0f, L"%.2f"),
};

static const MenuRow BeyondUltraRows[]
{
    Heading(L"BEYOND ULTRA"),
    Enumeration(L"Geometry", PREF_BEYONDULTRAGEOMETRY, "BeyondUltra", "BeyondUltraGeometry", BeyondUltraValues, BeyondUltraLabels),
    Enumeration(L"Shadows",  PREF_BEYONDULTRASHADOWS,  "BeyondUltra", "BeyondUltraShadows",  BeyondUltraShadowValues, BeyondUltraShadowLabels),
    Enumeration(L"Terrain",  PREF_BEYONDULTRATERRAIN,  "BeyondUltra", "BeyondUltraTerrain",  BeyondUltraValues, BeyondUltraLabels),
};

static const MenuRow GameplayRows[]
{
    Heading(L"GAMEPLAY"),
    Boolean(L"Remove Mouse Speed Cap", PREF_MOUSESPEEDCAP, "Gameplay", "RemoveMouseSpeedCap"),
    Range  (L"Mouse Look Sensitivity", PREF_MOUSELOOKSENSITIVITY, "Gameplay", "MouseLookSensitivity",
            VALUE_FLOAT, 10, 500, 5, 100.0f, L"%.2f"),
    BooleanFloat(L"Sprint Turn Modifier", PREF_SPRINTTURNMODIFIER, "Gameplay", "SprintTurnModifier"),
    Boolean(L"Aim Toggle",        PREF_AIMTOGGLE,        "Gameplay", "AimToggle"),
    Boolean(L"Sprint Toggle",     PREF_SPRINTTOGGLE,     "Gameplay", "SprintToggle"),
    Boolean(L"Limited Saving",    PREF_LIMITEDSAVING,    "Gameplay", "LimitedSaving"),
    Boolean(L"Console Autosaves", PREF_CONSOLEAUTOSAVES, "Gameplay", "ConsoleAutosaves"),
    Boolean(L"No Blinking Items", PREF_NOBLINKINGITEMS,  "Gameplay", "NoBlinkingItems"),
    Boolean(L"No Colored Signs",  PREF_NOCOLOREDSIGNS,   "Gameplay", "NoColoredSigns"),
};

// These four read zero on every line, and had done since the page was first built. Nothing to do
// with the values: they were the only float rows declared without a format, so they took the
// default of "%d" and handed it a double. A whole number of degrees as a double has thirty-two zero
// bits at the bottom, and a "%d" that reaches into a double's varargs slot finds exactly those, so
// every step printed 0, and the row that could not be read could not be moved either.
//
// So: hundredths, like every other float row on the page, and "%g" for the text. Hundredths because
// the value that matters is the one the ini already holds and the ini is free to hold 91.35, which
// whole degrees cannot carry. "%g" because it writes 91.35 as "91.35" and 95 as "95": the exact
// number where there is one, and a plain degree everywhere else.
//
// Fives rather than ones, since ninety-five steps between 45 and 140 is a row nobody can drive
// with an arrow key.
// The step builder keeps the ini's own value in place wherever it falls, so 91.35 sits between 90
// and 95 and one press either way lands on a round number and stays on the fives from then on. Both
// clamps are multiples of five already, so the whole ladder lines up with them.
static const MenuRow FieldOfViewRows[]
{
    Heading(L"FIELD OF VIEW"),
    Range(L"Field of View",     PREF_FIELDOFVIEW,          "FieldOfView", "FieldOfView",
          VALUE_FLOAT, 4500, 14000, 500, 100.0f, L"%g"),
    Range(L"Viewmodel FOV",     PREF_VIEWMODELFIELDOFVIEW, "FieldOfView", "ViewmodelFieldOfView",
          VALUE_FLOAT, 4500, 14000, 500, 100.0f, L"%g"),
    Range(L"Ironsight FOV",     PREF_IRONSIGHTFIELDOFVIEW, "FieldOfView", "IronsightFieldOfView",
          VALUE_FLOAT, 2000, 14000, 500, 100.0f, L"%g"),
    Range(L"Vehicle FOV",       PREF_VEHICLEFIELDOFVIEW,   "FieldOfView", "VehicleFieldOfView",
          VALUE_FLOAT, 4500, 14000, 500, 100.0f, L"%g"),
};

static const MenuRow ControllerRows[]
{
    Heading(L"CONTROLLER"),
    Range  (L"Look Sensitivity", PREF_CONTROLLERLOOKSENSITIVITY, "Controller", "ControllerLookSensitivity",
            VALUE_FLOAT, 0, 200, 5, 100.0f, L"%.2f"),
    Boolean(L"Aim Assist",         PREF_AIMASSIST,          "Controller", "AimAssist"),
    Boolean(L"Vibration",          PREF_VIBRATION,          "Controller", "Vibration"),
    Boolean(L"Controller Prompts", PREF_CONTROLLERPROMPTS,  "Controller", "ControllerPrompts"),
    Boolean(L"Aim Toggle",         PREF_AIMTOGGLECONTROLLER, "Controller", "AimToggle"),
};

static const MenuRow ContentRows[]
{
    Heading(L"CONTENT UNLOCKS"),
    Boolean(L"Predecessor Tapes", PREF_PREDECESSORTAPES, "ContentUnlocks", "PredecessorTapesUnlock"),
    Boolean(L"Machetes",          PREF_MACHETES,         "ContentUnlocks", "MachetesUnlock"),
};

static const MenuRow GeneralRows[]
{
    Heading(L"GENERAL"),
    Boolean    (L"Skip Intro",            PREF_SKIPINTRO,           "General", "SkipIntro"),
    Boolean    (L"Skip Title Screen",     PREF_SKIPTITLESCREEN,     "General", "SkipTitleScreen"),
    Enumeration(L"Skip Tutorials",        PREF_SKIPTUTORIALS,       "General", "SkipTutorials", SkipTutorialsValues, SkipTutorialsLabels),
    Enumeration(L"CPU Affinity",          PREF_CPUAFFINITY,         "General", "CpuAffinity",   CpuAffinityValues, CpuAffinityLabels),
    Boolean    (L"High Precision Timer",  PREF_HIGHPRECISIONTIMER,  "General", "HighPrecisionTimer"),
    Boolean    (L"Skip System Detection", PREF_SKIPSYSTEMDETECTION, "General", "SkipSystemDetection"),
    Boolean    (L"Large Address Aware",   PREF_LARGEADDRESSAWARE,   "General", "LargeAddressAware"),
};

// Ticks here rather than Yes/No, both because these are switches rather than settings and because
// it is the clearest place to see the two-value control wearing its other face.
static const MenuRow DebugRows[]
{
    Heading(L"DEBUG"),
    Boolean(L"Invincibility",       PREF_DEBUGINVINCIBILITY,    "Debug", "Invincibility"),
    Boolean(L"Infinite Ammo",       PREF_DEBUGINFINITEAMMO,     "Debug", "InfiniteAmmo"),
    Boolean(L"Unlock All Weapons",  PREF_DEBUGUNLOCKALLWEAPONS, "Debug", "UnlockAllWeapons"),
    Boolean(L"Noclip",              PREF_DEBUGNOCLIP,           "Debug", "Noclip"),
    Boolean(L"Freecam",             PREF_DEBUGFREECAM,          "Debug", "Freecam"),
    Range  (L"Diamonds",            PREF_DEBUGDIAMONDS,         "Debug", "Diamonds", VALUE_INT, 0, 999, 25),
};

// The pair the percentages are a percentage of, and the list they produce.
//
// The base is the ini's InternalResolutionX/Y read from the file rather than from what is in force,
// so a row already moved this session does not become the thing the next percentage is measured
// against. An unset pair means the window the game is drawing into, measured rather than assumed,
// because borderless makes the window and the video options disagree and only one of them is what
// the player is looking at.
//
// Twenty per cent of a 4K base is a frame the engine is happy with; twenty per cent of 640x480 is
// one it is not, and two hundred per cent of 15360x8640 is a render target D3D9 cannot describe.
// So the list is walked outwards from a hundred and stops at the first step whose pixels fall
// outside what can be carried. A hundred is always offered: it is the pair itself and is valid by
// construction.
static void SettleRenderResolutionChoices()
{
    auto nBaseW = JackalFixSettings.GetFileInt(PREF_INTERNALRESOLUTIONX);
    auto nBaseH = JackalFixSettings.GetFileInt(PREF_INTERNALRESOLUTIONY);

    // Unset means the window, and a hundred per cent of it is written back as unset rather than as
    // the pixel count, so the pair keeps meaning "follow the window" when it is left where it was.
    const auto bWindowBase = nBaseW <= 0 || nBaseH <= 0;

    if (bWindowBase)
    {
        RECT client{};
        auto hWnd = JackalFixGameWindow();

        if (hWnd != nullptr && GetClientRect(hWnd, &client)
            && client.right > client.left && client.bottom > client.top)
        {
            nBaseW = static_cast<int>(client.right - client.left);
            nBaseH = static_cast<int>(client.bottom - client.top);
        }
        else
        {
            nBaseW = GetSystemMetrics(SM_CXSCREEN);
            nBaseH = GetSystemMetrics(SM_CYSCREEN);
        }
    }

    if (nBaseW <= 0 || nBaseH <= 0)
    {
        nBaseW = 1920;
        nBaseH = 1080;
    }

    nRenderChoices = 0;

    // Even numbers of pixels. An odd render target is what the engine's own half resolution passes
    // trip over.
    auto Pixels = [](int nBase, int nPercent) { return (nBase * nPercent + 50) / 100 & ~1; };

    auto Fits = [&](int nPercent)
    {
        const auto nW = Pixels(nBaseW, nPercent);
        const auto nH = Pixels(nBaseH, nPercent);
        return nW >= nRenderPixelMinW && nH >= nRenderPixelMinH
            && nW <= nRenderPixelMax && nH <= nRenderPixelMax;
    };

    auto nLowest = 100;
    for (auto nPercent = 100 - nRenderScaleStep; nPercent >= nRenderScaleFloor; nPercent -= nRenderScaleStep)
    {
        if (!Fits(nPercent))
            break;
        nLowest = nPercent;
    }

    auto nHighest = 100;
    for (auto nPercent = 100 + nRenderScaleStep; nPercent <= nRenderScaleCeiling; nPercent += nRenderScaleStep)
    {
        if (!Fits(nPercent))
            break;
        nHighest = nPercent;
    }

    // Exactly sixteen by nine, whichever of the two the base came from.
    if (nBaseW * 9 == nBaseH * 16 && nRenderChoices < nMaxRenderChoices)
    {
        RenderChoiceX[nRenderChoices] = nConsoleWidth;
        RenderChoiceY[nRenderChoices] = nConsoleHeight;
        swprintf(RenderChoiceText[nRenderChoices], nRenderLabelLength, L"%dx%d (console)",
                 nConsoleWidth, nConsoleHeight);
        RenderChoiceLabels[nRenderChoices] = RenderChoiceText[nRenderChoices];
        nRenderChoices++;
    }

    for (auto nPercent = nLowest; nPercent <= nHighest && nRenderChoices < nMaxRenderChoices;
         nPercent += nRenderScaleStep)
    {
        const auto bBase = nPercent == 100 && bWindowBase;

        RenderChoiceX[nRenderChoices] = bBase ? 0 : Pixels(nBaseW, nPercent);
        RenderChoiceY[nRenderChoices] = bBase ? 0 : Pixels(nBaseH, nPercent);
        swprintf(RenderChoiceText[nRenderChoices], nRenderLabelLength, L"%d%% (%dx%d)", nPercent,
                 Pixels(nBaseW, nPercent), Pixels(nBaseH, nPercent));
        RenderChoiceLabels[nRenderChoices] = RenderChoiceText[nRenderChoices];
        nRenderChoices++;
    }

    for (auto& row : DisplayRows)
    {
        if (row.nPref == PREF_INTERNALRESOLUTIONX)
            row.nValueCount = nRenderChoices;
    }
}

struct MenuSection
{
    const MenuRow* pRows;
    size_t         nRows;
};

#define JACKALFIX_SECTION(a) { a, std::size(a) }

static const MenuSection MenuSections[]
{
    JACKALFIX_SECTION(DisplayRows),
    JACKALFIX_SECTION(GraphicsRows),
    JACKALFIX_SECTION(BeyondUltraRows),
    JACKALFIX_SECTION(GameplayRows),
    JACKALFIX_SECTION(FieldOfViewRows),
    JACKALFIX_SECTION(ControllerRows),
    JACKALFIX_SECTION(ContentRows),
    JACKALFIX_SECTION(GeneralRows),
    JACKALFIX_SECTION(DebugRows),
};

// ------------------------------------------------------------------------------------------------
// What the mod ships with.
//
// This is what DEFAULT restores, and it is deliberately the values in the ini as distributed rather
// than whatever the ini happens to hold now. The button means "put it back to how it came", and
// reading the user's own file for that would make it mean nothing.
//
// Everything is in the units the row works in, so a float carried in fixed point is written scaled:
// a saturation of 0.5 at a scale of a hundred is fifty, and a sensitivity of 1.0 is a hundred.

struct PrefDefault
{
    Pref nPref;
    int  nValue;
};

static const PrefDefault PrefDefaults[]
{
    { PREF_DISPLAYMODE,             2   },
    { PREF_INTERNALRESOLUTIONX,     0   },
    { PREF_INTERNALRESOLUTIONY,     0   },
    { PREF_SCALINGFILTER,           1   },
    { PREF_MAXFRAMERATE,            1   },

    { PREF_ANISOTROPICFILTERING,    16  },
    { PREF_X360GAMMA,               1   },
    { PREF_NORIMLIGHTING,           0   },
    { PREF_SATURATION,              50  }, // 0.50

    { PREF_BEYONDULTRAGEOMETRY,     0   },
    { PREF_BEYONDULTRASHADOWS,      0   },
    { PREF_BEYONDULTRATERRAIN,      0   },

    { PREF_MOUSESPEEDCAP,           1   },
    { PREF_MOUSELOOKSENSITIVITY,    100 }, // 1.00
    { PREF_SPRINTTURNMODIFIER,      0   },
    { PREF_AIMTOGGLE,               1   },
    { PREF_SPRINTTOGGLE,            1   },
    { PREF_LIMITEDSAVING,           0   },
    { PREF_CONSOLEAUTOSAVES,        1   },
    { PREF_NOBLINKINGITEMS,         0   },
    { PREF_NOCOLOREDSIGNS,          0   },

    { PREF_FIELDOFVIEW,             9131 }, // 91.31
    { PREF_VIEWMODELFIELDOFVIEW,    7500 }, // 75.00
    { PREF_IRONSIGHTFIELDOFVIEW,    5985 }, // 59.85
    { PREF_VEHICLEFIELDOFVIEW,      9131 }, // 91.31

    { PREF_CONTROLLERLOOKSENSITIVITY, 100 },
    { PREF_AIMASSIST,               1   },
    { PREF_VIBRATION,               1   },
    { PREF_CONTROLLERPROMPTS,       1   },
    { PREF_AIMTOGGLECONTROLLER,     1   },

    { PREF_PREDECESSORTAPES,        1   },
    { PREF_MACHETES,                1   },

    { PREF_SKIPINTRO,               1   },
    { PREF_SKIPTITLESCREEN,         1   },
    { PREF_SKIPTUTORIALS,           0   },
    { PREF_CPUAFFINITY,             0   },
    { PREF_UTILISATION,             1   },
    { PREF_HIGHPRECISIONTIMER,      1   },
    { PREF_SKIPSYSTEMDETECTION,     1   },
    { PREF_LARGEADDRESSAWARE,       1   },

    { PREF_DEBUGINVINCIBILITY,      0   },
    { PREF_DEBUGINFINITEAMMO,       0   },
    { PREF_DEBUGUNLOCKALLWEAPONS,   0   },
    { PREF_DEBUGNOCLIP,             0   },
    { PREF_DEBUGFREECAM,            0   },
    { PREF_DEBUGDIAMONDS,           0   },
};

// ------------------------------------------------------------------------------------------------
// The slot plan.
//
// Every declared row is given a slot, and slot S is drawn on line S modulo the number of lines.
// The plan is built once: a row takes the next slot whose line kind it can use, blanks fill
// anything skipped, and a heading is pushed to the next page rather than being left stranded on
// the last line of one. Scrolling is then a matter of choosing which run of lines to hand to the
// engine.
//
// Generous, and bounded by the declarations above rather than by anything at runtime.
static constexpr size_t nMaxSlots = 256;

struct MenuSlot
{
    const MenuRow* pRow; // null for a blank
};

static MenuSlot Slots[nMaxSlots]{};
static size_t   nSlots = 0;
static size_t   nPages = 0;

// One entry per declared row, in slot order, holding the value the user has chosen but not yet
// applied. Rows scroll out of the window and their controls are destroyed with them, so the value
// has to live somewhere that outlives the control or a change made on one page would be lost the
// moment the next was drawn.
static int  PendingValues[nMaxSlots]{};
static bool bPendingValid = false;

static WidgetKind WantedKind(const MenuRow& row)
{
    return row.nKind == ROW_SLIDER ? WIDGET_SLIDER : WIDGET_ARROWS;
}

static bool LineTakes(size_t nSlot, const MenuRow& row)
{
    return RowLines[nSlot % nRowLines].nKind == WantedKind(row);
}

// Whether any line at all can carry this kind of row. A row whose kind the layout does not provide
// would otherwise send the search below off the end of the plan, filling it with blanks, which is
// how a single misdeclared slider could swallow the entire menu.
static bool AnyLineTakes(const MenuRow& row)
{
    for (size_t i = 0; i < nRowLines; i++)
    {
        if (RowLines[i].nKind == WantedKind(row))
            return true;
    }
    return false;
}

static void PlanSlots()
{
    if (nSlots != 0)
        return;

    for (const auto& section : MenuSections)
    {
        if (section.nRows == 0)
            continue;

        // A heading is worth a line only if the first of its own rows can follow it on the same
        // page, otherwise that row ends up beneath the wrong title. Only the first is asked about:
        // a section longer than a page has to run over onto the next one regardless, and requiring
        // all of it to fit would push the heading down for ever.
        size_t nHeading = nSlots;
        for (size_t i = 1; i < section.nRows; i++)
        {
            const auto& first = section.pRows[i];
            if (!AnyLineTakes(first))
                continue;

            size_t nTarget = nHeading + 1;
            while (nTarget < nMaxSlots && !LineTakes(nTarget, first))
                nTarget++;

            if (nTarget / nRowLines != nHeading / nRowLines)
                nHeading = (nHeading / nRowLines + 1) * nRowLines;
            break;
        }

        while (nSlots < nHeading && nSlots < nMaxSlots)
            Slots[nSlots++] = { nullptr };

        if (nSlots >= nMaxSlots)
            break;

        Slots[nSlots++] = { &section.pRows[0] };

        for (size_t i = 1; i < section.nRows; i++)
        {
            const auto& row = section.pRows[i];

            // Dropped rather than shown wrong. A row the layout cannot draw is a declaration to
            // fix, and the log is where that gets noticed.
            if (!AnyLineTakes(row))
            {
                continue;
            }

            while (nSlots < nMaxSlots && !LineTakes(nSlots, row))
                Slots[nSlots++] = { nullptr };

            if (nSlots >= nMaxSlots)
                break;

            Slots[nSlots++] = { &row };
        }
    }

    // Round out the last page so the window never runs off the end of the plan.
    while (nSlots % nRowLines != 0 && nSlots < nMaxSlots)
        Slots[nSlots++] = { nullptr };

    nPages = nSlots / nRowLines;
}

// ------------------------------------------------------------------------------------------------
// What was actually built, so Apply and the page turn can walk it without asking the engine's row
// map, which is a std::map whose operator[] inserts a null on a miss and would hand us one to
// dereference.

struct BuiltRow
{
    size_t nSlot;
    void*  pControl;
};

// Defined with the navigation, which is the only other thing that moves the selection.
static void SettleSelection(void* pListBox, int nFrom, int nDirection);
static void RefreshIfStale(void* pPage);
static void ShowPage(void* pPage, size_t nPage, bool bKeepOnScreenValues, int nTurnDirection);

static BuiltRow BuiltRows[nMaxLines]{};
static size_t   nBuiltRows = 0;
static size_t   nCurrentPage = 0;

// ------------------------------------------------------------------------------------------------
// Everything the log below walks is engine memory whose shape is inferred rather than known, so it
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

// ------------------------------------------------------------------------------------------------
// Giving the layout more rows.
//
// The page can only ever show as many value rows as options.mgb declares widgets for, and it
// declares eight. Nothing at runtime can add a ninth: the engine's duplication machinery only
// instantiates the Area family, and a settings row is a magma::Element, so the object server hands
// back null and the duplicator dereferences it. That was worth finding out the hard way once.
//
// So the rows are added where they belong, in the layout. Not by shipping a modified options.mgb,
// but by patching the bytes on their way into the parser. The file is read into one contiguous
// heap buffer before a single byte of it is parsed, so substituting that buffer is substituting
// the file, and nothing reaches the disk.
//
// The format cooperates: it is a purely sequential object graph with no absolute offsets, no size
// fields and no checksum, so bytes can be inserted anywhere provided every enclosing count is
// corrected. Four edits do it: the new row containers, their entries in the page's name map, and
// the two counts above them.
//
// Every offset is found by walking the file rather than written down, because the localised copies
// are different sizes. If any anchor fails to resolve the buffer is left exactly as it was.

static constexpr uint32_t nMagmaVersion = 0x001EAB90;
static constexpr uint8_t  nMagmaSentinel = 0xAB;

// Element type indices, as the file stores them.
static constexpr uint8_t nTypePage         = 0x65;
static constexpr uint8_t nTypeRowContainer = 0x3A;

// Inside a row container's 91 byte record.
static constexpr size_t   nRowRecordSize   = 91;
static constexpr ptrdiff_t nRowName        = 1;
static constexpr ptrdiff_t nRowId          = 20;
static constexpr ptrdiff_t nRowX           = 49;
static constexpr ptrdiff_t nRowY           = 51;
static constexpr ptrdiff_t nRowScaleOne    = 53; // two 1.0f, and a fingerprint for the record shape
static constexpr ptrdiff_t nRowTemplate    = 73;

// A name map entry: key, tag, then a five component path.
static constexpr size_t   nMapEntrySize = 31;
static constexpr uint32_t nVariantLink  = 0x12;
static constexpr uint8_t  nPathTypeIndex = 66; // magma::Focusable, what the path ends at

// Where the extra rows start, and how far apart. The eight the file ships with run 225 to 420 at a
// spacing of 28, so these carry straight on from them.
static constexpr uint16_t nExtraRowFirstY = 448;
static constexpr uint16_t nExtraRowStep   = 28;

// How many rows to add to the layout. Nine puts the last one clear of the Back / Default / Apply
// prompts along the bottom with the lift applied, which is as many as the page has room for.
static constexpr int nExtraRows = 9;

static const uint32_t nOnePointZero = 0x3F800000;

static uint32_t Crc32Of(const char* pText)
{
    static uint32_t Table[256]{};
    static bool bBuilt = false;
    if (!bBuilt)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            auto nEntry = i;
            for (int nBit = 0; nBit < 8; nBit++)
                nEntry = (nEntry & 1) ? (nEntry >> 1) ^ 0xEDB88320 : nEntry >> 1;
            Table[i] = nEntry;
        }
        bBuilt = true;
    }

    uint32_t nCrc = 0xFFFFFFFF;
    for (auto p = reinterpret_cast<const uint8_t*>(pText); *p != 0; p++)
        nCrc = Table[(nCrc ^ *p) & 0xFF] ^ (nCrc >> 8);
    return ~nCrc;
}

static uint32_t ReadU32(const uint8_t* p, size_t nOffset) { uint32_t v; memcpy(&v, p + nOffset, 4); return v; }
static uint16_t ReadU16(const uint8_t* p, size_t nOffset) { uint16_t v; memcpy(&v, p + nOffset, 2); return v; }
static void WriteU32(uint8_t* p, size_t nOffset, uint32_t v) { memcpy(p + nOffset, &v, 4); }
static void WriteU16(uint8_t* p, size_t nOffset, uint16_t v) { memcpy(p + nOffset, &v, 2); }

// Steps over one { key, tag, payload }, returning where the next begins, or zero if the tag is one
// this build does not know, in which case the stream has been lost and nothing further is trusted.
static uint32_t StepMapEntry(const uint8_t* pFile, uint32_t nLength, uint32_t nAt)
{
    if (nAt + 8 > nLength)
        return 0;

    auto nTag = ReadU32(pFile, nAt + 4);
    nAt += 8;

    switch (nTag)
    {
    case 0x02: case 0x07: nAt += 4; break;
    case 0x0C:            nAt += 1; break;
    case 0x13:            nAt += 8; break;
    case 0x14:                       break;
    case 0x10:
        if (nAt + 4 > nLength)
            return 0;
        nAt += 4 + ReadU32(pFile, nAt);
        break;
    case 0x11: case 0x12: case 0x15:
    {
        if (nAt + 2 > nLength)
            return 0;
        auto nCount = ReadU16(pFile, nAt);
        nAt += nCount == 0 ? 2 : 3 + 4 * static_cast<uint32_t>(nCount);
        break;
    }
    default:
        return 0;
    }

    return nAt <= nLength ? nAt : 0;
}

// Steps over a name map: a count, then that many { key, tag, payload }. The payload's length
// depends on the tag, and an unrecognised tag means the walk has lost the stream, which is
// reported rather than guessed at, because guessing here is how a file gets silently corrupted.
static bool WalkNameMap(const uint8_t* pFile, uint32_t nLength, uint32_t nOffset, uint32_t& nEnd)
{
    if (nOffset + 4 > nLength)
        return false;

    auto nEntries = ReadU32(pFile, nOffset);
    if (nEntries > 4096)
        return false;

    auto nAt = nOffset + 4;
    for (uint32_t i = 0; i < nEntries; i++)
    {
        if (nAt + 8 > nLength)
            return false;

        auto nTag = ReadU32(pFile, nAt + 4);
        nAt += 8;

        switch (nTag)
        {
        case 0x02: case 0x07: nAt += 4; break;                     // int, float
        case 0x0C:            nAt += 1; break;                     // bool
        case 0x13:            nAt += 8; break;                     // a pair of words
        case 0x14:                       break;                    // nothing at all
        case 0x10:                                                 // length prefixed ascii
            if (nAt + 4 > nLength)
                return false;
            nAt += 4 + ReadU32(pFile, nAt);
            break;
        case 0x11: case 0x12: case 0x15:                           // a path
        {
            if (nAt + 2 > nLength)
                return false;
            auto nCount = ReadU16(pFile, nAt);
            nAt += nCount == 0 ? 2 : 3 + 4 * static_cast<uint32_t>(nCount);
            break;
        }
        default:
            return false;
        }

        if (nAt > nLength)
            return false;
    }

    nEnd = nAt;
    return true;
}

// Everything the patch needs to know about where things are, found by walking rather than written
// down: the localised copies of this file are different sizes and none of these offsets would hold.
struct PackageSites
{
    uint32_t nRowsBegin;
    uint32_t nMapCount;    // the page's map entry count
    uint32_t nMapEnd;      // where a new entry is appended
    uint32_t nChildCount;  // the page's child count
    uint32_t nRowsEnd;     // where a new row container is appended
    uint32_t nSliderRow;   // the record for the row carrying the layout's lone slider, or 0
    uint16_t nRowX;        // this page's rows, as opposed to another page's rows of the same name
    uint16_t nLastRowY;
};

static bool IsOptionsPackage(const uint8_t* pFile, uint32_t nLength)
{
    if (nLength < 0x400 || memcmp(pFile, "MAGMA", 5) != 0)
        return false;
    if (pFile[8] != nMagmaSentinel || ReadU32(pFile, 9) != nMagmaVersion)
        return false;

    // The package's own name, at a position derived from the type table rather than assumed.
    auto nBody = 0x0F + 4u * (static_cast<uint32_t>(pFile[0x0E]) - 1);
    auto nPackageName = nBody + 65u * 4u;
    if (nPackageName + 4 > nLength)
        return false;

    return ReadU32(pFile, nPackageName) == Crc32Of("options");
}

static bool FindPackageSites(const uint8_t* pFile, uint32_t nLength, PackageSites& Sites)
{
    Sites = {};

    // The page whose map names the settings rows. Its name narrows it down; the shape of what
    // follows settles it, because the same name appears more than once in the file.
    auto nPageName = Crc32Of("p_option_game");
    uint32_t nPage = 0;

    for (uint32_t p = 0; p + 40 < nLength && nPage == 0; p++)
    {
        if (pFile[p] != nTypePage || ReadU32(pFile, p + 1) != nPageName)
            continue;

        uint32_t nMapEnd = 0;
        if (!WalkNameMap(pFile, nLength, p + 5, nMapEnd))
            continue;
        if (nMapEnd + 13 > nLength || pFile[nMapEnd] != 0)
            continue;
        if (ReadU32(pFile, nMapEnd + 1) != 30 || ReadU32(pFile, nMapEnd + 5) != 0)
            continue;

        auto nChildren = ReadU32(pFile, nMapEnd + 9);
        if (nChildren == 0 || nChildren > 4096)
            continue;

        nPage = p;
        Sites.nMapCount = p + 5;
        Sites.nMapEnd = nMapEnd;
        Sites.nChildCount = nMapEnd + 9;
    }

    if (nPage == 0)
        return false;

    // The rows themselves. A type byte alone is not enough to recognise one, since plenty of other
    // records begin with the same value, so the search is anchored on the names this page's own map
    // points at, which is the one thing that definitely identifies a settings row of this page.
    uint32_t RowNames[32]{};
    size_t nNames = 0;
    {
        auto nEntries = ReadU32(pFile, Sites.nMapCount);
        auto nAt = Sites.nMapCount + 4;
        for (uint32_t i = 0; i < nEntries && nNames < std::size(RowNames); i++)
        {
            if (nAt + 11 > nLength)
                break;

            auto nTag = ReadU32(pFile, nAt + 4);
            if (nTag == nVariantLink && ReadU16(pFile, nAt + 8) == 5)
                RowNames[nNames++] = ReadU32(pFile, nAt + 19);

            nAt = StepMapEntry(pFile, nLength, nAt);
            if (nAt == 0)
                break;
        }
    }

    if (nNames == 0)
        return false;

    auto IsRowName = [&](uint32_t nName)
    {
        for (size_t i = 0; i < nNames; i++)
            if (RowNames[i] == nName)
                return true;
        return false;
    };

    // The map names more than one kind of row, and the row names themselves are reused by other
    // pages in the same file, so neither a name nor a coordinate identifies this page's block on
    // its own. What does identify it is that it is a contiguous run of records sharing one x that
    // covers most of the map's rows: a stray single record fails that, and the other page's
    // identically named block sits later in the file.
    static constexpr size_t nMinimumRun = 4;

    uint32_t nFirstRow = 0;
    auto nTemplateList = Crc32Of("p_list");

    for (uint32_t r = Sites.nMapEnd; r + nRowRecordSize <= nLength; r++)
    {
        if (pFile[r] != nTypeRowContainer || !IsRowName(ReadU32(pFile, r + nRowName)))
            continue;
        if (ReadU32(pFile, r + nRowScaleOne) != nOnePointZero || ReadU32(pFile, r + nRowScaleOne + 4) != nOnePointZero)
            continue;

        auto nX = ReadU16(pFile, r + nRowX);
        uint32_t Seen[32]{};
        size_t nSeen = 0;
        auto nAt = r;

        while (nAt + nRowRecordSize <= nLength
            && pFile[nAt] == nTypeRowContainer
            && IsRowName(ReadU32(pFile, nAt + nRowName))
            && ReadU16(pFile, nAt + nRowX) == nX)
        {
            auto nName = ReadU32(pFile, nAt + nRowName);
            bool bKnown = false;
            for (size_t i = 0; i < nSeen && !bKnown; i++)
                bKnown = Seen[i] == nName;
            if (!bKnown && nSeen < std::size(Seen))
                Seen[nSeen++] = nName;

            nAt += nRowRecordSize;
        }

        if (nSeen < nMinimumRun)
        {
            r = nAt - 1; // past this run, not one byte into it
            continue;
        }

        nFirstRow = r;
        Sites.nRowsBegin = r;
        Sites.nRowX = nX;
        Sites.nRowsEnd = nAt;
        break;
    }

    if (nFirstRow == 0)
        return false;

    for (auto nRow = nFirstRow; nRow < Sites.nRowsEnd; nRow += nRowRecordSize)
    {
        Sites.nLastRowY = ReadU16(pFile, nRow + nRowY);
        if (ReadU32(pFile, nRow + nRowTemplate) != nTemplateList)
            Sites.nSliderRow = nRow;
    }

    return Sites.nRowsEnd > nFirstRow && Sites.nLastRowY != 0;
}

// One row container, built to the shape the file's own records have. Every byte not named here is
// zero in the originals, and the handful of constants in the middle are copied verbatim because
// their meaning is not established, which is also why the caller checks them against a real record
// before trusting any of this.
static void MakeRowRecord(uint8_t* pOut, const char* pName, uint16_t nX, uint16_t nY, uint32_t nId)
{
    memset(pOut, 0, nRowRecordSize);
    pOut[0] = nTypeRowContainer;
    WriteU32(pOut, nRowName, Crc32Of(pName));
    pOut[11] = 1;
    WriteU32(pOut, 16, 1);
    WriteU32(pOut, nRowId, nId);
    WriteU32(pOut, 37, 0xFFFFFFFF);
    WriteU16(pOut, nRowX, nX);
    WriteU16(pOut, nRowY, nY);
    WriteU32(pOut, nRowScaleOne, nOnePointZero);
    WriteU32(pOut, nRowScaleOne + 4, nOnePointZero);
    pOut[66] = 0x01;
    WriteU32(pOut, 67, 0xEC705196);
    pOut[71] = 0xE5;
    pOut[72] = 0x01;
    WriteU32(pOut, nRowTemplate, Crc32Of("p_list"));
    WriteU32(pOut, 77, 1);
    pOut[90] = 0xFF;
}

static void MakeMapEntry(uint8_t* pOut, const char* pKey, const char* pRowName)
{
    WriteU32(pOut, 0, Crc32Of(pKey));
    WriteU32(pOut, 4, nVariantLink);
    WriteU16(pOut, 8, 5);
    pOut[10] = nPathTypeIndex;
    WriteU32(pOut, 11, Crc32Of("options"));
    WriteU32(pOut, 15, Crc32Of("p_option_game"));
    WriteU32(pOut, 19, Crc32Of(pRowName));
    WriteU32(pOut, 23, Crc32Of("p_list"));
    WriteU32(pOut, 27, Crc32Of("l_setting"));
}

// Names of the rows the patch adds. Kept here so the layout side can ask for the same ones.
static void ExtraRowName(char* pOut, size_t nSize, int nIndex) { snprintf(pOut, nSize, "p_setting_%d", 11 + nIndex); }
static void ExtraRowKey (char* pOut, size_t nSize, int nIndex) { snprintf(pOut, nSize, "SETTING_%d",   11 + nIndex); }

// Builds the replacement image. Returns the length, or zero if anything did not add up, in which
// case the caller leaves the original bytes alone and the page runs on its eight rows.
static uint32_t BuildPatchedPackage(const uint8_t* pFile, uint32_t nLength, uint8_t* pOut, uint32_t nOutCapacity)
{
    PackageSites Sites{};
    if (!FindPackageSites(pFile, nLength, Sites))
    {
        return 0;
    }

    // The record shape is copied from a real row, so it is worth proving one exists in the form the
    // builder assumes before emitting eight more in that form.
    auto nSample = Sites.nRowsEnd - nRowRecordSize;
    if (ReadU32(pFile, nSample + 67) != 0xEC705196 || pFile[nSample + 71] != 0xE5 || pFile[nSample + 90] != 0xFF)
    {
        return 0;
    }

    auto nRows = static_cast<uint32_t>(nExtraRows);
    auto nAddedRows = nRows * static_cast<uint32_t>(nRowRecordSize);
    auto nAddedMap  = nRows * static_cast<uint32_t>(nMapEntrySize);
    auto nNewLength = nLength + nAddedRows + nAddedMap;
    if (nNewLength > nOutCapacity)
        return 0;

    // Copied forward in three runs, so the two insertions land without disturbing anything else.
    uint32_t nWritten = 0;
    auto Append = [&](const void* pData, uint32_t nBytes) { memcpy(pOut + nWritten, pData, nBytes); nWritten += nBytes; };

    Append(pFile, Sites.nMapEnd);

    for (uint32_t i = 0; i < nRows; i++)
    {
        char szKey[24]{}, szRow[24]{};
        ExtraRowKey(szKey, sizeof(szKey), static_cast<int>(i));
        ExtraRowName(szRow, sizeof(szRow), static_cast<int>(i));

        uint8_t Entry[nMapEntrySize]{};
        MakeMapEntry(Entry, szKey, szRow);
        Append(Entry, nMapEntrySize);
    }

    Append(pFile + Sites.nMapEnd, Sites.nRowsEnd - Sites.nMapEnd);

    for (uint32_t i = 0; i < nRows; i++)
    {
        char szRow[24]{};
        ExtraRowName(szRow, sizeof(szRow), static_cast<int>(i));

        // The id is not understood. It is stable for a name within a page but differs for the same
        // name across pages, so it is not a hash of the name alone. Something unique is the best
        // that can be said for what goes here.
        auto nId = Crc32Of(szRow) ^ 0x5A5A5A5Au;

        uint8_t Record[nRowRecordSize]{};
        MakeRowRecord(Record, szRow, Sites.nRowX,
            static_cast<uint16_t>(nExtraRowFirstY + nExtraRowStep * i - nLiftUnits), nId);
        Append(Record, nRowRecordSize);
    }

    Append(pFile + Sites.nRowsEnd, nLength - Sites.nRowsEnd);

    // The rows are lifted here, in the layout, rather than by moving each value widget at run time.
    // A widget's own coordinate is an offset inside its container, so moving the widget slid it out
    // from under the container that owns it, which is what the mouse hit-tests against, and why
    // the arrows could only be worked with the keyboard. Moving the container instead keeps the two
    // together and costs nothing, since the layout is already being rewritten.
    for (auto nRow = Sites.nRowsBegin + nAddedMap; nRow < Sites.nRowsEnd + nAddedMap; nRow += nRowRecordSize)
    {
        auto nY = ReadU16(pOut, nRow + nRowY);
        WriteU16(pOut, nRow + nRowY, static_cast<uint16_t>(nY - nLiftUnits));
    }

    // The header carries sixty-five pool sizes, and the parser allocates every object it creates
    // out of them. They are pre-sized to exactly what the stock file needs, and the allocator that
    // draws on them pops a free list head without checking it, so eight extra elements against
    // unchanged counts does not fail, it dereferences null part way through loading the package.
    //
    // Which pool belongs to which class is not established, so every non-empty one gets headroom.
    // An unused entry costs one object's worth of memory and nothing else.
    auto nPools = 0x0Fu + 4u * (static_cast<uint32_t>(pFile[0x0E]) - 1);
    auto nHeadroom = nRows * 8 + 32;
    uint32_t nRaised = 0;
    for (uint32_t i = 0; i < 65; i++)
    {
        auto nAt = nPools + i * 4;
        if (nAt + 4 > nLength)
            break;

        auto nCount = ReadU32(pOut, nAt);
        if (nCount == 0)
            continue;

        WriteU32(pOut, nAt, nCount + nHeadroom);
        nRaised++;
    }

    // Then the two counts above what was inserted. The child count sits after the map, so it has
    // moved by exactly the bytes the map grew by.
    WriteU32(pOut, Sites.nMapCount, ReadU32(pOut, Sites.nMapCount) + nRows);
    WriteU32(pOut, Sites.nChildCount + nAddedMap, ReadU32(pOut, Sites.nChildCount + nAddedMap) + nRows);

    // The row the layout gives a slider is of no use to this page: nothing here pairs with one, and
    // it has been a blank line in the middle of every screen. Pointed at the ordinary arrows
    // template it becomes another usable row. Its entry in the map is repointed to match, or the
    // path would still lead to the slider.
    if (Sites.nSliderRow != 0)
    {
        auto nSlider = Sites.nSliderRow + (Sites.nSliderRow >= Sites.nMapEnd ? nAddedMap : 0);
        WriteU32(pOut, nSlider + nRowTemplate, Crc32Of("p_list"));

        auto nSliderName = ReadU32(pOut, nSlider + nRowName);
        for (uint32_t nAt = Sites.nMapCount + 4; nAt + nMapEntrySize <= Sites.nMapEnd + nAddedMap; nAt++)
        {
            if (ReadU32(pOut, nAt + 4) != nVariantLink || ReadU16(pOut, nAt + 8) != 5)
                continue;
            if (ReadU32(pOut, nAt + 19) != nSliderName)
                continue;

            WriteU32(pOut, nAt + 23, Crc32Of("p_list"));
            WriteU32(pOut, nAt + 27, Crc32Of("l_setting"));
            break;
        }
    }

    return nWritten;
}

// ------------------------------------------------------------------------------------------------
// Substituting the bytes on the way in.
//
// The hook sits on the buffered reader itself rather than on anything above it. That is deliberate:
// the first attempt hooked the package loader and walked from the archive down to the reader, got
// one offset in that chain wrong, and silently did nothing, since the reader pointer came out null
// and every test after it fell through. Here the reader IS the object the hook is called on, so
// there is no chain to get wrong.
//
// It runs after the original, by which point the reader has pulled the whole file into one heap
// block and copied the caller's bytes out of it. Swapping the block then is safe because the patch
// only ever inserts well past the header, so everything read so far is identical in both copies -
// and the read position is left alone so the parse carries on exactly where it was.

static constexpr ptrdiff_t nReaderBuffer   = 0x04;
static constexpr ptrdiff_t nReaderBuffered = 0x08;
static constexpr ptrdiff_t nReaderPosition = 0x0C;
static constexpr ptrdiff_t nReaderSize     = 0x10;

// Only worth looking at a file that has barely been read, which is every file exactly once and
// costs nothing for the ninety-odd packages that are not this one.
static constexpr uint32_t nReaderEarly = 32;

// Not the allocator the pages are built with. That is a different one, and pairing it with the free
// this block will eventually meet would corrupt the heap on the way out rather than the way in.
static constexpr uint32_t nEngineAllocSlot = 0x00FB6440;

using ReaderRead_t  = void  (__fastcall*)(void* pReader, void* pEdx, void* pDestination, uint32_t nBytes);
using EngineAlloc_t = void* (__cdecl*)(size_t nSize, int nFlags);

static SafetyHookInline ReaderReadHook{};

// Read at the moment of use, never cached. The slot holds a placeholder until the engine installs
// its real allocator, and a plugin initialises long before that, so caching it here would keep hold
// of something that hands back null for ever, which is exactly what the first attempt did.
static EngineAlloc_t EngineAllocator()
{
    if (hDunia == nullptr)
        return nullptr;

    auto ppAllocator = reinterpret_cast<EngineAlloc_t*>(reinterpret_cast<uint8_t*>(hDunia) + nEngineAllocSlot);
    if (!IsReadable(ppAllocator, sizeof(void*)))
        return nullptr;

    auto pAllocator = *ppAllocator;
    return PointsIntoDunia(reinterpret_cast<void*>(pAllocator)) ? pAllocator : nullptr;
}

// Ours already, or the original? The added rows carry names the stock file does not, so one of them
// is the marker. Cheaper than tracking which blocks we handed out, and correct even if the engine
// reloads the package into a block we have seen before.
static bool AlreadyPatched(const uint8_t* pFile, uint32_t nLength)
{
    char szKey[24]{};
    ExtraRowKey(szKey, sizeof(szKey), 0);
    auto nMarker = Crc32Of(szKey);

    for (uint32_t i = 0; i + 4 <= nLength; i++)
    {
        if (ReadU32(pFile, i) == nMarker)
            return true;
    }
    return false;
}

static void __fastcall ReaderRead(void* pReader, void* pEdx, void* pDestination, uint32_t nBytes)
{
    ReaderReadHook.fastcall(pReader, pEdx, pDestination, nBytes);

    // Everything past here is rejected in silence for other files. Once it is established that this
    // is the options layout, every remaining way out says so. The first version of this reported
    // nothing at all when it gave up, which made a build that did nothing indistinguishable from a
    // build that never ran.
    if (SafeRead<uint32_t>(pReader, nReaderPosition) > nReaderEarly)
        return;
    if (SafeRead<uint8_t>(pReader, nReaderBuffered) == 0)
        return;

    auto pBuffer = SafeRead<uint8_t*>(pReader, nReaderBuffer);
    auto nLength = SafeRead<uint32_t>(pReader, nReaderSize);
    if (pBuffer == nullptr || nLength < 0x400 || !IsReadable(pBuffer, nLength))
        return;
    if (!IsOptionsPackage(pBuffer, nLength))
        return;
    if (AlreadyPatched(pBuffer, nLength))
        return;

    auto Allocate = EngineAllocator();
    if (Allocate == nullptr)
        return;

    auto nCapacity = nLength + static_cast<uint32_t>(nExtraRows) * (nRowRecordSize + nMapEntrySize) + 64;
    auto pPatched = static_cast<uint8_t*>(Allocate(nCapacity, 0));
    if (pPatched == nullptr)
    {
        return;
    }

    auto nPatched = BuildPatchedPackage(pBuffer, nLength, pPatched, nCapacity);
    if (nPatched == 0)
        return;

    // Handed over rather than copied over: the archive owns this block now and frees it with the
    // allocator it came from. The original is left behind deliberately, since freeing it would mean
    // calling a second engine function on trust, and one leaked copy of a fifty kilobyte file per
    // menu build is not worth that.
    *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pReader) + nReaderBuffer) = pPatched;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pReader) + nReaderSize) = nPatched;

}

// ------------------------------------------------------------------------------------------------
// Reshaping the page.
//
// Two things are wrong with the stock layout for our purposes: it starts too far down, and it
// carries eight value widgets when the settings list wants twice that. Both are fixable at runtime
// and neither needs options.mgb touched.
//
// Moving is the easy half. A widget's position lives on the magma::State hanging off widget+08h
// and is written only by the keyframe evaluator, so a poke holds until the next evaluation and is
// then undone. The engine's own answer to that is the lock mask at widget+0Ch:
// Widget::OnPropertyChanged sets exactly these bits when a script assigns POSITION.Y, and every
// state evaluator tests them before writing its channel. Setting the bits ourselves and then
// writing the state reaches the same place the engine would, which is why the move is permanent
// rather than a race with the animation.
//
// More rows is the harder half, and is done with the engine's own duplication machinery rather than
// by hand: an AreaLink says "duplicate this node into that parent", the manager walks the subtree,
// and every child, widget and nested link comes across correctly typed. magma::ListBox::CreateItem
// does exactly this at runtime for its per-item widgets, so the path is well travelled. All that is
// left to us afterwards is a name, because the value-row bind finds its widget by name and the
// document's name table is a flat CRC-keyed list that new entries can be appended to.
//
// Every engine function this needs is checked against its own first bytes before it is called. A
// mismatch, whether a different build of Dunia or anything else unexpected, disables the extra rows
// and leaves the page working on its stock eight rather than crashing.

// magma::Widget.
static constexpr ptrdiff_t nWidgetState    = 0x08;
static constexpr ptrdiff_t nWidgetLockMask = 0x0C;
static constexpr uint32_t  nLockPositionY  = 0x00000C04; // pos.y, rect.top, rect.bottom

// magma::State. A ListBox and an AreaInstance carry a ScaleState, where +24h is x and +26h is y. A
// Text carries a TextState, which is a RectState, and there +24h..+2Ah are left, right, top and
// bottom instead. The two are not interchangeable and the difference is silent, so each kind of
// widget is written in its own terms rather than through one helper.
static constexpr ptrdiff_t nStateScaleY     = 0x26;
static constexpr ptrdiff_t nStateRectTop    = 0x28;
static constexpr ptrdiff_t nStateRectBottom = 0x2A;

// magma::Element, and the property container every named object hangs its name table from.
static constexpr ptrdiff_t nElementParent     = 0x04;
static constexpr ptrdiff_t nElementName       = 0x08;
static constexpr ptrdiff_t nOwnerProperties   = 0x0C;
static constexpr ptrdiff_t nElementWidget     = 0x14;

// The page's title text widget, resolved by the base widget pass alongside the row list.
static constexpr ptrdiff_t nPageTitleWidget   = 0x10;

// The container a cloned child is appended to keeps its child vector here.
static constexpr ptrdiff_t nContainerChildrenEnd = 0x18;

// magma::FullLink, the payload a name-table entry of tag 12h carries. Its dword array alternates
// resolved node pointers with path hashes, and a name resolves to the last node in it, at index 2m
// where m is (count - 1) / 2.
static constexpr size_t    nFullLinkSize     = 0x1C;
static constexpr ptrdiff_t nFullLinkBegin    = 0x08;
static constexpr ptrdiff_t nFullLinkEnd      = 0x0C;
static constexpr ptrdiff_t nFullLinkCapacity = 0x10;
static constexpr ptrdiff_t nVariantTag       = 0x04;
static constexpr ptrdiff_t nVariantPayload   = 0x08;
static constexpr int32_t   nVariantLinkTag   = 0x12;

// The bar the page heading and the rule under it both live in.
// The panel the heading, its rule and the row list all sit inside.
//
// Worth writing down what a dump of this document's name table showed, because it cost a round trip
// to find out: the table holds twenty-one entries and every one of them is a magma::ListBox, the
// value widgets and the row list itself. There is no text element in it at all, and no title bar.
// So neither this name nor a_title_bar can ever resolve here, and the rule under the heading is not
// reachable the way the rest of this file reaches things. The engine finds it by walking a parent's
// children, which is a different lookup entirely (0x10535B70 / 0x10108C70) and the thing to reach
// for next rather than another name.
static const char szNavPanel[] = "p_menu_nav";

// The bar the page heading and the rule under it share.
static const char szTitleBar[] = "a_title_bar";

using Crc32_t             = uint32_t (__cdecl*)(uint32_t* pOut, const char* pText);
using NameLookup_t        = void*    (__fastcall*)(void* pContainer, void* pEdx, uint32_t nCrc);

// Moves the selection, refreshing the ink on the row it left and the row it landed on. Needed
// because a heading is an ordinary selectable row to the engine, and at the top of the first page
// there is nowhere for a step-over to step to.
using SetSelection_t      = int      (__fastcall*)(void* pListBox, void* pEdx, int nIndex, int bRefresh, int bScroll);

// The highlight is not the selection. +D4h is which row is selected; +D0h is which row is lit, and
// they are set by different calls, which is why moving the selection stepped through the rows
// perfectly while nothing changed on screen. This is the one that lights a row, and it is also what
// a mouse click uses to light a value widget, which is how the highlight left the list in the first
// place. Index -1 releases it.
using SetHighlight_t      = void     (__fastcall*)(void* pListBox, void* pEdx, void* pFocusable, uint32_t nUser, int nIndex, int nChannel);

// Recomputes a row's cached appearance from its flags. This is what puts a heading back into the
// greyed ink the moment the pointer passes over it, the disabled flag being one of its inputs, so
// the correction has to happen on the way out of it rather than once when the rows are built.
using RefreshRow_t        = void     (__fastcall*)(void* pListBox, void* pEdx, int nIndex, int nAnimate);
static constexpr int nHighlightKeyboard = 1;
static constexpr int nHighlightPointer  = 0;

// The engine's own lookup, which is not the document name table this file uses elsewhere: it scans
// a container's child vector. That is the only way to reach anything that is not a value widget,
// since the name table on this page holds nothing but those.
//
// The name is a std::string, so it is built and destroyed with the game's own pair rather than
// hand-rolled: the buffer is at +04h and the object carries an allocator at +00h that these two
// touch and a different destructor would dereference.
using FindArea_t    = void* (__stdcall*)(void* pContainer, void* pName);
using StringMake_t  = void* (__fastcall*)(void* pString, void* pEdx, const char* pText);
using StringFree_t  = void  (__fastcall*)(void* pString, void* pEdx);

static Crc32_t             Crc32             = nullptr;
static NameLookup_t        NameLookup        = nullptr;
static SetSelection_t      SetSelection      = nullptr;
static SetHighlight_t      SetHighlight      = nullptr;

// Every widget reaches its input handler through its own Focusable, which arrives as the sender, so
// the row list's is caught the first time it is asked to do anything, and kept for the times the
// keys arrive somewhere else.
static void*    pRowListFocusable = nullptr;

// What an ordinary row's cached appearance looks like, taken from a real row on the page so a
// heading can be given the same one back.
static uint32_t nOrdinaryItemState = 0;
static bool     bOrdinaryItemState = false;

static SafetyHookInline RefreshRowHook{};
static SafetyHookInline TextDrawHook{};
static SafetyHookInline TextDrawPlainHook{};
static SafetyHookInline ImageDrawHook{};
static SafetyHookInline DrawRowHook{};
static SafetyHookInline ListDrawHook{};

// The value that currently holds the light, so it can be given up the moment a key arrives that the
// value has no use for.
static void*    pValueFocusable = nullptr;
static int      nValueLine = -1;
static uint32_t nInputUser = 0;
static FindArea_t          FindArea          = nullptr;
static StringMake_t        StringMake        = nullptr;
static StringFree_t        StringFree        = nullptr;

// Two hex digits, or a ? standing for a byte that is an absolute address and so not fixed.
static bool MatchesSignature(const uint8_t* pCode, const char* pSignature)
{
    auto Hex = [](char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };

    for (size_t i = 0; pSignature[i] != '\0'; )
    {
        if (pSignature[i] == ' ')
        {
            i++;
            continue;
        }

        if (pSignature[i] == '?')
        {
            pCode++;
            i++;
            continue;
        }

        auto nHigh = Hex(pSignature[i]);
        auto nLow  = Hex(pSignature[i + 1]);
        if (nHigh < 0 || nLow < 0)
            return false;

        if (*pCode++ != static_cast<uint8_t>(nHigh * 16 + nLow))
            return false;
        i += 2;
    }
    return true;
}

// Resolved by offset and then proved by its own first bytes. Dunia loads at its preferred base, so
// the offset is the address; if it ever did not, the signatures carry no absolute immediates and
// would still match. What the check really guards against is a different build of the DLL, where
// being wrong means calling into the middle of something else.
static void* ResolveEngineFunction(const char* pName, uint32_t nOffset, const char* pSignature)
{
    if (hDunia == nullptr)
        return nullptr;

    auto pCode = reinterpret_cast<uint8_t*>(hDunia) + nOffset;
    if (!IsReadable(pCode, 32) || !MatchesSignature(pCode, pSignature))
    {
        return nullptr;
    }
    return pCode;
}

// Both are only ever used to turn a name into an element. Each is resolved by offset and then
// proved by its own first bytes; if either fails, nothing is moved and the page draws where the
// layout put it rather than somewhere half-corrected.
static void ResolveLayoutSupport()
{
    Crc32      = reinterpret_cast<Crc32_t>     (ResolveEngineFunction("Crc32",      0x00AA7150, "80 3D ? ? ? ? 00 0F 84 96 00 00 00 C6 05"));
    NameLookup = reinterpret_cast<NameLookup_t>(ResolveEngineFunction("Names::Get", 0x00AD2B30, "56 8B 71 08 57 8B 7C 24 0C 33 D2 EB 03 8D 49 00"));

    SetHighlight = reinterpret_cast<SetHighlight_t>(ResolveEngineFunction("ListBox::SetHighlight", 0x00A9D1A0,
        "56 8B F1 8B 86 D0 00 00 00 57 8B 7C 24 14 3B F8 89 BE D0 00"));

    SetSelection = reinterpret_cast<SetSelection_t>(ResolveEngineFunction("ListBox::SetSelection", 0x00A9C800,
        "51 53 56 8B F1 8B 8E 94 00 00 00 57 8B BE D4 00 00 00 85 FF"));

    FindArea   = reinterpret_cast<FindArea_t>  (ResolveEngineFunction("FindArea",   0x00535B70, "8B 44 24 08 8B 54 24 04 56 50 52 E8 ? ? ? ? 8B F0 85 F6"));
    StringMake = reinterpret_cast<StringMake_t>(ResolveEngineFunction("String::ctor", 0x000BD1D0, "51 56 8B F1 33 C0 57 8D 4C 24 0B 88 44 24 0B 89 46 14 8D 46"));
    StringFree = reinterpret_cast<StringFree_t>(ResolveEngineFunction("String::dtor", 0x000BCF90, "51 56 8B F1 83 7E 18 10 72 0D 8B 46 04 50 FF 15 ? ? ? ?"));

}

// Done once per document. A document is keyed by page-name hash across the whole process and
// outlives a menu session, so the moves persist with it, which is why this is guarded on the
// document rather than repeated on every open.
static void* pPreparedDocument = nullptr;

// The same resolution the engine performs, done here so a name can be turned into an element
// without building a std::string for its own lookup.
static void* FindElementByName(void* pDocument, const char* pName)
{
    if (Crc32 == nullptr || NameLookup == nullptr)
        return nullptr;

    auto pContainer = SafeRead<void*>(pDocument, nOwnerProperties);
    if (pContainer == nullptr)
        return nullptr;

    uint32_t nCrc = 0;
    Crc32(&nCrc, pName);

    auto pVariant = NameLookup(pContainer, nullptr, nCrc);
    if (pVariant == nullptr || SafeRead<int32_t>(pVariant, nVariantTag) != nVariantLinkTag)
        return nullptr;

    auto pLink = SafeRead<void*>(pVariant, nVariantPayload);
    if (pLink == nullptr)
        return nullptr;

    auto pBegin = SafeRead<uint32_t*>(pLink, nFullLinkBegin);
    auto pEnd   = SafeRead<uint32_t*>(pLink, nFullLinkEnd);
    if (pBegin == nullptr || pEnd <= pBegin)
        return nullptr;

    auto nCount = static_cast<size_t>(pEnd - pBegin);
    if (nCount <= 2)
        return nullptr;

    auto nIndex = ((nCount - 1) >> 1) * 2;
    return nIndex < nCount ? reinterpret_cast<void*>(pBegin[nIndex]) : nullptr;
}

// An instance's own children hang off a sub-tree struct, and the container the lookup wants is one
// dereference in from it.
static constexpr ptrdiff_t nInstanceChildren = 0x3C;

// The three state classes a moveable thing might carry. They disagree about what +26h means: it is
// y on one family and the RIGHT edge on the other, so the class is read rather than assumed, and
// an unrecognised one is left alone instead of being written to on a guess.
static constexpr uint32_t nRectStateVTable  = 0x00EE6D74;
static constexpr uint32_t nPosStateVTable   = 0x00EEBD24;
static constexpr uint32_t nScaleStateVTable = 0x00EEA17C;
static constexpr uint32_t nLockRectY = 0x00000C00; // rect top and bottom
static constexpr uint32_t nLockPosY  = 0x00000004; // pos y

static constexpr ptrdiff_t nStateRectRight = 0x26;

static void* FindAreaByName(void* pContainer, const char* pName)
{
    if (FindArea == nullptr || StringMake == nullptr || StringFree == nullptr || pContainer == nullptr)
        return nullptr;

    uint8_t Name[0x1C]{};
    StringMake(Name, nullptr, pName);
    auto pFound = FindArea(pContainer, Name);
    StringFree(Name, nullptr);
    return pFound;
}

// Moves an instance up the page, in whichever terms its state class understands.
static void MoveInstanceUp(void* pInstance, int nUp, const char* pWhat)
{
    auto pState = SafeRead<void*>(pInstance, nWidgetState);
    if (pInstance == nullptr || pState == nullptr)
        return;

    auto nVTable = SafeRead<uint32_t>(pState, 0);
    auto nBase = reinterpret_cast<uint32_t>(hDunia);
    auto& nLock = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pInstance) + nWidgetLockMask);

    if (nVTable == nBase + nRectStateVTable)
    {
        // Both edges, by the same amount; moving one of them stretches the box instead.
        nLock |= nLockRectY;
        auto pTop    = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(pState) + nStateRectTop);
        auto pBottom = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(pState) + nStateRectBottom);
        *pTop    = static_cast<int16_t>(*pTop - nUp);
        *pBottom = static_cast<int16_t>(*pBottom - nUp);
    }
    else if (nVTable == nBase + nPosStateVTable || nVTable == nBase + nScaleStateVTable)
    {
        nLock |= nLockPosY;
        auto pY = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(pState) + nStateRectRight);
        *pY = static_cast<int16_t>(*pY - nUp);
    }
    else
    {
    }
}

// A row's own widget is a magma::ListBox, so its position is the x and y pair on a ScaleState.
static void MoveRowWidget(void* pWidget, int16_t nY)
{
    auto pState = SafeRead<void*>(pWidget, nWidgetState);
    if (pWidget == nullptr || pState == nullptr)
        return;

    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pWidget) + nWidgetLockMask) |= nLockPositionY;
    *reinterpret_cast<int16_t*>(static_cast<uint8_t*>(pState) + nStateScaleY) = nY;
}

static bool RowWidgetY(void* pWidget, int16_t& nOut)
{
    auto pState = SafeRead<void*>(pWidget, nWidgetState);
    if (pWidget == nullptr || pState == nullptr)
        return false;

    nOut = SafeRead<int16_t>(pState, nStateScaleY);
    return true;
}

// The title is a magma::Text, whose state is a RectState: top and bottom rather than a y, and both
// have to move or the text is drawn into a box that has grown instead of moved.
static void MoveTitle(void* pTitle, int nUp)
{
    auto pState = SafeRead<void*>(pTitle, nWidgetState);
    if (pTitle == nullptr || pState == nullptr)
        return;

    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pTitle) + nWidgetLockMask) |= nLockPositionY;

    auto pTop    = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(pState) + nStateRectTop);
    auto pBottom = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(pState) + nStateRectBottom);
    *pTop    = static_cast<int16_t>(*pTop - nUp);
    *pBottom = static_cast<int16_t>(*pBottom - nUp);
}

// Done once per document. The document is keyed by page-name hash across the whole process and
// outlives a menu session, so the clones and the moves persist with it, which is why this is
// guarded on the document rather than run on every open.
static void PrepareLayout(void* pPage)
{
    auto pDocument = SafeRead<void*>(pPage, nPageDocument);
    if (pDocument == nullptr || pDocument == pPreparedDocument)
        return;

    // Every line the layout will actually give us, asked for by name. The first eight are the ones
    // options.mgb ships with; anything past them exists only because the patch above put it there,
    // so the list stops at the first name that does not resolve. That makes the two halves of this
    // work independent: if the package patch did not take, the page quietly runs on eight rows.
    nRowLines = 0;
    void*   pRowWidgets[nMaxLines]{};
    int16_t nRowY[nMaxLines]{};

    for (size_t i = 0; i < nMaxLines; i++)
    {
        if (i < std::size(StockLines))
            snprintf(LineNames[i], sizeof(LineNames[0]), "%s", StockLines[i].pWidgetName);
        else
            ExtraRowKey(LineNames[i], sizeof(LineNames[0]), static_cast<int>(i - std::size(StockLines)));

        auto pElement = FindElementByName(pDocument, LineNames[i]);
        auto pWidget = pElement != nullptr ? SafeRead<void*>(pElement, nElementWidget) : nullptr;
        if (!RowWidgetY(pWidget, nRowY[i]))
            break;

        pRowWidgets[i] = pWidget;
        RowLines[i] = { LineNames[i], WIDGET_ARROWS };
        nRowLines++;
    }

    // The second line carries the layout's lone slider. The package patch repoints it at the
    // ordinary arrows template, so it is only unusable when that patch did not happen.
    if (nRowLines <= std::size(StockLines) && nRowLines > 1)
        RowLines[1].nKind = WIDGET_UNUSABLE;

    // Everything that already exists moves up together. A row's own coordinate is an offset inside
    // its container rather than a position on the page, so the lift is applied to it rather than to
    // a measured absolute, which is also why every one of them reads as zero to begin with.
    // Left where they are. The lift now lives in the layout, on the containers these sit inside, so
    // the only reason to touch a value widget at run time is to park it off the page when the line
    // it belongs to has no value on it.
    for (size_t i = 0; i < nRowLines; i++)
    {
        LineWidgets[i] = pRowWidgets[i];
        LineY[i] = nRowY[i];
    }

    // There are no duplicates, and there cannot be.
    //
    // The intent here was to stamp out more rows at runtime from the engine's own duplication
    // machinery. That machinery only instantiates the Area family (Area, Page, Button, CheckBox
    // and Cursor), and provably nothing else derives from Area in this DLL. A settings row is none
    // of those: both l_setting, which a SETTING_N name resolves to, and the p_setting_N container
    // above it are magma::Elements, and the object server returns null for an Element. BeginClone
    // dereferences that null without checking, which is the access violation at 0x10AEC57C.
    //
    // Cloning the nearest Area up the path does not help either. It would copy the container of a
    // row rather than a row, the copy is never attached to anything (the only thing that makes a
    // duplicate reachable is retargeting an AreaLink that already exists), and every clone of one
    // source is given the same name, because the system is a template-instancing facility for list
    // rows and its output is deliberately never looked up by name.
    //
    // So the page runs on the eight rows the layout declares. More than eight has to come from the
    // layout carrying more than eight, which is a different piece of work entirely.

    // The heading, the rule under it and the row labels are all children of p_menu_nav, and
    // children are drawn relative to their parent, so moving the panel moves the three of them
    // together and in step. That is the whole reason for doing it this way: chasing the rule as an
    // individual element found something named a_title_bar, moved it, reported that it had moved,
    // and changed nothing on screen, twice. The panel is the thing whose position demonstrably
    // governs what is drawn here, because the row list inside it is what has been moving all
    // along.
    //
    // The value widgets on the right are not in this panel, hanging off the page's own content
    // instead, so they are still moved one by one, above.
    auto pPanel = FindAreaByName(pDocument, szNavPanel);
    if (pPanel != nullptr)
    {
        MoveInstanceUp(pPanel, nLiftUnits, "the nav panel");
    }
    else
    {
        // No panel: move what can be reached on its own and accept that the rule stays put.

        if (auto pTitleBar = FindAreaByName(pDocument, szTitleBar))
            MoveInstanceUp(pTitleBar, nLiftUnits, "the title bar");
        else
            MoveTitle(SafeRead<void*>(pPage, nPageTitleWidget), nLiftUnits);

        auto pList = SafeRead<void*>(pPage, nPageList);
        int16_t nListY = 0;
        if (RowWidgetY(pList, nListY))
            MoveRowWidget(pList, static_cast<int16_t>(nListY - nLiftUnits));
    }

    pPreparedDocument = pDocument;
}

// ------------------------------------------------------------------------------------------------
// Settings in, settings out.
//
// The settings store keeps floats and ints in the same variant, so a row's integer traffic is
// converted at the edges rather than everywhere.

static int SettingToRow(const MenuRow& row)
{
    if (row.nPref == Pref::COUNT)
        return 0;
    if (row.nValue == VALUE_FLOAT)
        return static_cast<int>(JackalFixSettings.GetFloat(row.nPref) * row.fScale + 0.5f);
    return JackalFixSettings.GetInt(row.nPref);
}

// Settings the menu applies but does not write.
//
// The four FOV values are the ones a player is most likely to have set by hand, and the number
// they set is usually not one of the round ones a row can offer: 91.31 sits between 90 and 95.
// Writing the row's choice to the file would replace it, and there would be no getting it back. So
// these are applied to the running game and held for the session while the file keeps saying what
// it said.
static bool IsSessionOnly(Pref nPref)
{
    switch (nPref)
    {
    case PREF_FIELDOFVIEW:
    case PREF_VIEWMODELFIELDOFVIEW:
    case PREF_IRONSIGHTFIELDOFVIEW:
    case PREF_VEHICLEFIELDOFVIEW:

    // The render resolution row moves these for the run and leaves the ini alone, so the pair the
    // player wrote stays what a hundred per cent means. Writing them would make each move the base
    // for the next, and the row would walk away from the resolution it was asked for.
    case PREF_INTERNALRESOLUTIONX:
    case PREF_INTERNALRESOLUTIONY:
        return true;
    default:
        return false;
    }
}

// Whether a row can do anything at all as the game currently stands.
//
// The three Beyond Ultra rows edit one quality block each, the ultrahigh geometry, shadow and
// terrain blocks, and the game only reads the block belonging to the level chosen in its own
// display options. So with Shadow on High, Beyond Ultra Shadows is a row that moves and changes
// nothing, and the honest thing is to say so rather than let it be set. Each row answers for its
// own section: Geometry on Ultra High and Shadow on High greys the second and leaves the first
// alone.
//
// Unavailable is not the same as off. The value in the ini is untouched, the row still shows what
// it holds, and it comes back by itself the moment the game's own setting is raised.
static bool IsRowAvailable(const MenuRow& row)
{
    switch (row.nPref)
    {
    case PREF_BEYONDULTRAGEOMETRY: return JackalFixGeometryIsUltra();
    case PREF_BEYONDULTRASHADOWS:  return JackalFixShadowsAreUltra();
    case PREF_BEYONDULTRATERRAIN:  return JackalFixTerrainIsUltra();
    default:                       return true;
    }
}

// The three answers as one value, so a page can be told that the ground moved under it while it was
// being built.
static uint32_t BeyondUltraAvailability()
{
    return (JackalFixGeometryIsUltra() ? 1u : 0u)
        | (JackalFixShadowsAreUltra() ? 2u : 0u)
        | (JackalFixTerrainIsUltra() ? 4u : 0u);
}

// The same as SettingToRow, but against the file rather than against what is in force. For a
// session-only row these part company the moment the row is moved, and this is the one that has to
// stay on the ladder: it is the number the player wrote, and nothing writes it back.
static int FileToRow(const MenuRow& row)
{
    if (row.nValue == VALUE_FLOAT)
        return static_cast<int>(JackalFixSettings.GetFileFloat(row.nPref) * row.fScale + 0.5f);
    return JackalFixSettings.GetFileInt(row.nPref);
}

// The value this row came shipped with, or the setting as it stands if the row is not one of the
// ones the mod declares a default for.
static int DefaultForRow(const MenuRow& row)
{
    for (const auto& entry : PrefDefaults)
    {
        if (entry.nPref == row.nPref)
            return entry.nValue;
    }
    return SettingToRow(row);
}

// The chosen index of an enumeration, by value rather than by position: the ini may hold something
// the list does not offer, in which case the first entry stands in.
static int NearestEnumValue(const MenuRow& row, int nValue)
{
    for (uint32_t i = 0; i < row.nValueCount; i++)
    {
        if (row.pValues[i] == nValue)
            return nValue;
    }
    return row.nValueCount != 0 ? row.pValues[0] : nValue;
}

// ------------------------------------------------------------------------------------------------
// Generated step lists.
//
// A range row is drawn with arrows over a list of steps built as the row is built. The value the
// ini actually holds is inserted in its place if the steps miss it, so a FieldOfView of 91.31
// shows as 91 rather than snapping to the nearest step and quietly rewriting a value the user
// never touched.
//
// The engine deep copies both arrays as the row is built, so this storage only has to survive the
// call, but it is kept per visible line anyway, which costs little and makes the lifetime obvious.

static constexpr size_t nMaxSteps = 48;
// Wide enough for the longest step text any row produces, which is the render resolution's
// "200% (15360x8640)".
static constexpr size_t nStepTextLength = 24;

static int            StepValues[nMaxLines][nMaxSteps]{};
static const wchar_t* StepLabels[nMaxLines][nMaxSteps]{};
static wchar_t        StepText  [nMaxLines][nMaxSteps][nStepTextLength]{};

// Leaves a row holding the one choice it already has, so the arrows have nowhere to go. Reuses the
// per line step storage, which the row APIs deep copy out of anyway. A value that is not in its own
// list is left alone rather than replaced, since there would be nothing to put in its place.
static uint32_t CollapseToCurrent(size_t nLine, int nCurrent,
                                  const int*& pValues, const wchar_t* const*& pLabels, uint32_t nCount)
{
    for (uint32_t i = 0; i < nCount; i++)
    {
        if (pValues[i] != nCurrent)
            continue;

        const auto* pLabel = pLabels[i];
        StepValues[nLine][0] = nCurrent;
        StepLabels[nLine][0] = pLabel;

        pValues = StepValues[nLine];
        pLabels = StepLabels[nLine];
        return 1;
    }

    return nCount;
}

static uint32_t BuildSteps(size_t nLine, const MenuRow& row, int nCurrent)
{
    uint32_t nCount = 0;

    auto Emit = [&](int nValue)
    {
        if (nCount >= nMaxSteps)
            return;

        StepValues[nLine][nCount] = nValue;

        if (row.nValue == VALUE_FLOAT)
        {
            swprintf(StepText[nLine][nCount], nStepTextLength, row.pFormat, static_cast<double>(nValue) / row.fScale);
        }
        else
        {
            swprintf(StepText[nLine][nCount], nStepTextLength, row.pFormat, nValue);
        }

        if (row.nPref == PREF_SATURATION && nValue == DefaultForRow(row))
        {
            wchar_t szValue[nStepTextLength]{};
            wcsncpy_s(szValue, StepText[nLine][nCount], _TRUNCATE);
            swprintf(StepText[nLine][nCount], nStepTextLength, L"%s (Default)", szValue);
        }

        StepLabels[nLine][nCount] = StepText[nLine][nCount];
        nCount++;
    };

    // Values that belong on the ladder without being on it.
    //
    // One is what the row is set to: the ini is free to hold something the steps do not offer, and
    // dropping it would silently rewrite it on the next Apply. The other is what the file says,
    // which for a session-only row is a different number the moment the row is moved, and the one
    // that has to keep being offered, because it is the number the player wrote by hand and there
    // is no other way back to it. Both are placed in order among the steps rather than tacked on
    // the end, and either may coincide with a step or with the other, in which case it is only
    // shown once.
    int    Loose[2]{};
    size_t nLoose = 0;

    auto Offer = [&](int nValue)
    {
        for (size_t i = 0; i < nLoose; i++)
        {
            if (Loose[i] == nValue)
                return;
        }
        Loose[nLoose++] = nValue;
    };

    Offer(nCurrent);
    if (row.nPref != Pref::COUNT && IsSessionOnly(row.nPref))
        Offer(FileToRow(row));

    if (nLoose == 2 && Loose[0] > Loose[1])
    {
        auto nSwap = Loose[0];
        Loose[0] = Loose[1];
        Loose[1] = nSwap;
    }

    size_t nPlaced = 0;

    // Everything loose that sits below the step about to be drawn, followed by anything that is
    // that step, which the walk is about to draw, so it is not drawn twice.
    auto Settle = [&](int nStepValue)
    {
        while (nPlaced < nLoose && Loose[nPlaced] < nStepValue)
            Emit(Loose[nPlaced++]);
        while (nPlaced < nLoose && Loose[nPlaced] == nStepValue)
            nPlaced++;
    };

    auto nFrom = row.nMinimum;
    auto nStep = row.nStep > 0 ? row.nStep : 1;
    for (int nValue = nFrom; nValue <= row.nMaximum && nCount < nMaxSteps; nValue += nStep)
    {
        Settle(nValue);
        Emit(nValue);
    }

    // Above the top of the ladder, or the ladder ran out of room.
    while (nPlaced < nLoose && nCount < nMaxSteps)
        Emit(Loose[nPlaced++]);

    return nCount;
}

// ------------------------------------------------------------------------------------------------
// Reading and writing a row's value.
//
// The three control classes disagree on both the width of the value and where they keep the widget
// they bound, so each is asked in its own terms. A null widget means the name lookup failed when
// the row was built, and the getter would hand back an uninitialised field, so that case reports
// failure rather than a plausible looking zero.

/*
  Fading a row's value.

  The enabled flag handed to the row APIs only reaches the entry: Dunia+81D660 passes it straight to
  AddEntry and then builds the value control separately, which is why the label greys and the value
  beside it stays black. The note further down about row ink is about the row template, which is
  shared and has no per-row colour. This is a different object. Each value control keeps the widget
  it was bound to by name at +44h, +48h on a slider; that widget carries a magma State at +08h, and
  the State carries four colours at +44h, which is the same layout the controller prompt glyphs are
  already tinted through.

  The four are moved halfway to mid grey rather than set to a colour of my choosing. Whichever byte
  is the alpha and whatever order the rest are in, halfway to 80h desaturates a colour and takes the
  edge off an alpha, so the result reads as faded without the format having to be settled first.
*/
// nWidgetState is already declared with the rest of the widget layout further up. The rest is the
// magma tree, the same offsets the controller prompt glyphs are placed and tinted through.
/*
  Where a colour lives depends on the class, and getting that wrong is what took the game down
  twice.

  An ImageState keeps four of them at +44h, one per corner, which is where the controller prompt
  glyphs are tinted. A TextState does not: magma::Text's draw string, the Text vtable's +D8h slot at
  Dunia+AB4350, reads its colour from State+10h and treats State+44h as the shadow colour behind two
  enable bytes at +48h and +49h. Writing four dwords at +44h on a Text overwrote the
  shadow, both flags and four bytes past the end of the object, and the vtable of whatever the heap
  put next was gone, which is the call to zero in magma's draw both dumps ended on.

  So the offset is chosen by class and the write stays inside it.
*/
static constexpr ptrdiff_t nImageStateColour = 0x44;
static constexpr int nImageStateColourCount = 4;
static constexpr ptrdiff_t nTextStateColour = 0x10;
static constexpr ptrdiff_t nWidgetChildArea = 0x3C;
static constexpr ptrdiff_t nAreaChildBegin = 0x28;
static constexpr ptrdiff_t nAreaChildEnd = 0x2C;
static constexpr ptrdiff_t nNodeDrawable = 0x14;
static constexpr ptrdiff_t nNodeNameId = 0x08;
static constexpr size_t nMaxWidgetChildren = 64;
static constexpr int nMaxWidgetDepth = 4;

// Which magma classes these offsets are true of, by vtable. Walking the tree without asking is what
// took the game down: a child's drawable is not always a widget, and +08h on something else is not
// a State, so the four colour writes landed on a stranger's fields and the next call through what
// used to be its vtable went to zero. Nothing is read or written now until the object has said what
// it is, which is how the controller prompt module walks the same tree.
static constexpr ptrdiff_t nImageVtableRva = 0xEE6A04;
static constexpr ptrdiff_t nAreaInstanceVtableRva = 0xEE6BB4;
static constexpr ptrdiff_t nTextVtableRva = 0xEE63E4;

/*
  Fading a row's value, by which row is being drawn rather than by which object it is.

  The enabled flag handed to the row APIs only reaches the entry: Dunia+81D660 passes it to AddEntry
  and then builds the value control separately, which is why the label greys and the value beside it
  stays black.

  Walking to that value and colouring it does not work, and the log said why: all three unavailable
  rows reported the same magma::Text, 64D1D190 for every one of them. The list keeps one set of row
  drawables and re-poses it for each row on its way past, so no pointer can ever tell one row from
  another and any colour written into one of those objects belongs to every row at once.

  What can tell them apart is which row the list is drawing, and the list says so outright. Its
  Draw, the list vtable's 15th slot at Dunia+A9F590, loops over its visible cells calling
  Dunia+A9EC60 with the list as this, the first visible index at ListBox+CCh plus the step as the
  second argument, and the cell's position as the third. That second argument is a row index into
  the very same items array the appearance hook works from, Dunia+A9EC60 bounding it against
  ListBox+94h/+98h and taking items[index] from it, so the row being drawn is read rather than
  counted.

  The line is INHERITED rather than cleared by any list drawn inside a row. A value on this page is
  itself a CListBox, which is what "17 of 17 value widget(s) taken over" is counting and why they
  can be handed the list vtable at all, so the row's own value runs the same Dunia+A9EC60 one level
  down. Clearing on a foreign list is what left the value and the arrows black while the row's own
  artwork faded: the only three drawables the outer row ever reported were its background strips,
  and everything inside the value went past with no line at all. A value list is also caught by its
  own Draw against the line widget table, so it fades whether it is drawn inside the row or beside
  it.

  The parts then fade themselves on the way in. magma::Text takes its colour from State+10h, since
  Dunia+AB4D20 does MOV EDI,[ESI+08h] then MOV ECX,[EDI+10h] and hands that to the glyph call, with
  +44h only reached when one of the shadow bytes at +48h/+49h is set. magma::Image takes its four
  State+44h, which Image::Draw at Dunia+AB93F0 walks through LEA EBP,[ESI+44h].

  What is written is not a colour of my choosing. The engine already greys a row's label and the log
  says exactly how: the one Text an unavailable row draws for itself arrives at 32000000, plain
  black at an alpha of 32h, while the value beside it is FF000000 and the arrows are C8000000, both
  the same black at full strength. So the difference between a greyed label and a black value is the
  alpha alone, and inventing a grey to write over the colour was never going to match it; a flat
  B0B0B0 at the value's own FF came out as a harder, brighter grey than the label next to it.

  So the alpha is lowered to the label's and the colour is left alone. It is a ceiling rather than
  an assignment, which is what keeps the row's background strips out of it: they are authored at
  alpha zero, seen only under the selection, and an earlier pass that wrote an opaque grey over
  them turned every unavailable row into a white band. Nothing is ever made more visible than it
  was.
*/
static int nDrawingLine = -1;

// The alpha the engine itself greys a disabled label to, read off the row's own Text in the log. A
// row's parts are brought down to it and no further, so a value already fainter than this keeps
// what it had.
static constexpr uint32_t nFadedAlpha = 0x32;
static constexpr int nAlphaShift = 24;
static constexpr uint32_t nColourMask = 0x00FFFFFF;

// 83 EC 14 / PUSH ESI / MOV ESI,ECX / MOV EAX,[ESI+08h] / MOV ECX,[EAX+10h] / SHR ECX,18h.
static const char* const szListDrawPattern =
    "83 EC 14 56 8B F1 8B 46 08 8B 48 10 C1 E9 18 84 C9 57 75 22";

// 83 EC 1C / PUSH EBX / MOV EBX,ECX / MOV ECX,[EBX+94h]. The items array, which is the first
// thing it does with the index it was handed.
static const char* const szDrawRowPattern =
    "83 EC 1C 53 8B D9 8B 8B 94 00 00 00 85 C9 57 89 5C 24 14 75 04";

// 83 EC 2C / PUSH EBX / MOV EBX,[..] / PUSH ESI / PUSH EDI / MOV EDI,ECX / MOV ECX,[..] /
// MOV ESI,[EDI+08h]. The widget's State, which is where the four colours are read from.
static const char* const szImageDrawPattern =
    "83 EC 2C 53 8B 1D ? ? ? ? 56 57 8B F9 8B 0D ? ? ? ? 8B 77 08 E8";

/*
  magma::Text draws its string through one of two of its own methods, and the row's value goes
  through the other one.

  Text::Draw, the vtable's 15th slot at Dunia+A95500, takes the plain route, the one that calls +DCh
  at Dunia+AB4D20, whenever the text has no scale of its own and nothing to lay out; only the turned
  or measured case reaches +D8h, Dunia+AB4350. So hooking +D8h alone never saw a
  single one of these rows. Both read the colour out of State+10h and both are hooked.
*/
static const char* const szTextDrawPattern =
    "83 EC 5C 55 56 8B F1 8B 46 58 8B 68 10 57 8B 7E 08 D9 47 40 51 D9 5C 24 1C";

static const char* const szTextDrawPlainPattern =
    "83 EC 5C 53 55 56 8B F1 8B 46 58 8B 68 10 57 8B 7E 08 D9 47 40 51 D9 5C 24 20";

static uintptr_t DuniaRva(ptrdiff_t nOffset)
{
    return reinterpret_cast<uintptr_t>(hDunia) + nOffset;
}

static bool IsVTable(uint8_t* pDrawable, ptrdiff_t nRva)
{
    return IsReadable(pDrawable, sizeof(uintptr_t))
        && *reinterpret_cast<uintptr_t*>(pDrawable) == DuniaRva(nRva);
}

// A line whose row is there but cannot be used, which is the only thing that fades.
static bool IsLineUnavailable(int nLine)
{
    if (nLine < 0 || nRowLines == 0 || nLine >= static_cast<int>(nRowLines))
        return false;

    const auto nSlot = nCurrentPage * nRowLines + static_cast<size_t>(nLine);
    const auto* pRow = nSlot < nSlots ? Slots[nSlot].pRow : nullptr;
    return pRow != nullptr && pRow->nKind != ROW_HEADING && !IsRowAvailable(*pRow);
}

static size_t nTracedDraws = 0;
static constexpr size_t nMaxTracedDraws = 24;

/*
  Lent for one draw and taken straight back.

  The arrows are one pair of magma::Images for the whole list, re-posed per row the same way the
  row template is. The log has line 13 reading them back as C8B0B0B0, which is line 12's fade still
  sitting on them. Left like that, the first usable row drawn after an unavailable one would come
  out greyed too, and on a page whose Beyond Ultra rows are followed by anything with arrows that
  is exactly what would have happened.

  So the colour is put back the moment the drawable's own draw returns. Nothing shared is left
  holding a value that belongs to another row, and the write is only ever live for the one call that
  reads it.
*/
struct FadedState
{
    uint8_t*  pState = nullptr;
    ptrdiff_t nOffset = 0;
    int       nCount = 0;
    uint32_t  Saved[4]{};
};

// Writes the faded colour into a widget's State on its way into that widget's own draw. Bounded,
// and only once the object has agreed it is the class the offset belongs to. Four dwords at +44h
// on a Text is what took the game down twice.
static FadedState FadeForDraw(void* pWidget, ptrdiff_t nVtableRva, ptrdiff_t nOffset, int nCount)
{
    FadedState state;

    if (!IsLineUnavailable(nDrawingLine))
        return state;

    auto* pDrawable = static_cast<uint8_t*>(pWidget);
    if (!IsVTable(pDrawable, nVtableRva) || !IsReadable(pDrawable, nWidgetState + sizeof(void*)))
        return state;

    auto* pState = *reinterpret_cast<uint8_t**>(pDrawable + nWidgetState);
    if (!IsReadable(pState, nOffset + nCount * static_cast<ptrdiff_t>(sizeof(uint32_t))))
        return state;

    if (nTracedDraws < nMaxTracedDraws)
    {
        nTracedDraws++;
    }

    state.pState = pState;
    state.nOffset = nOffset;
    state.nCount = nCount;

    for (int i = 0; i < nCount; i++)
    {
        auto& nColour = *reinterpret_cast<uint32_t*>(pState + nOffset + i * sizeof(uint32_t));
        state.Saved[i] = nColour;

        const auto nAlpha = nColour >> nAlphaShift;
        if (nAlpha > nFadedAlpha)
            nColour = (nFadedAlpha << nAlphaShift) | (nColour & nColourMask);
    }

    return state;
}

static void RestoreAfterDraw(const FadedState& state)
{
    for (int i = 0; i < state.nCount; i++)
        *reinterpret_cast<uint32_t*>(state.pState + state.nOffset + i * sizeof(uint32_t)) = state.Saved[i];
}

static bool IsRowBound(const MenuRow& row, void* pControl)
{
    if (pControl == nullptr)
        return false;

    auto nOffset = row.nKind == ROW_SLIDER ? nSliderNamedWidget : nControlNamedWidget;
    return SafeRead<void*>(pControl, nOffset) != nullptr;
}

static bool ReadRowValue(const MenuRow& row, void* pControl, int& nOut)
{
    if (!IsRowBound(row, pControl))
        return false;

    auto ppVTable = *reinterpret_cast<uintptr_t**>(pControl);
    auto pValue = reinterpret_cast<GetRowValue_t>(ppVTable[nRowGetValueSlot])(pControl, nullptr);
    if (pValue == nullptr)
        return false;

    // The two-value control stores its choices as single bytes; the other two use ints.
    auto bTwoValue = row.nKind == ROW_BOOL || row.nKind == ROW_TICK;
    auto nValue = bTwoValue
        ? static_cast<int>(*static_cast<const uint8_t*>(pValue))
        : *static_cast<const int*>(pValue);

    // None of the three getters reports failure. Each returns a pointer into the control: to a
    // scratch field on success, and to a separate, never initialised field when the widget holds
    // no valid selection. So a plausible looking integer is not evidence of anything, and a value
    // outside what the row was built with is rejected rather than written to the user's ini.
    switch (row.nKind)
    {
    case ROW_BOOL:
    case ROW_TICK:
        if (nValue != 0 && nValue != 1)
            return false;
        break;

    case ROW_ENUM:
    {
        bool bKnown = false;
        for (uint32_t i = 0; i < row.nValueCount && !bKnown; i++)
            bKnown = row.pValues[i] == nValue;
        if (!bKnown)
            return false;
        break;
    }

    case ROW_RANGE:
    case ROW_SLIDER:
        if (nValue < row.nMinimum || nValue > row.nMaximum)
        {
            // A range row may legitimately carry a value from outside its steps, because the ini is
            // allowed to. Only reject what the row was never offered.
            return false;
        }
        break;

    default:
        return false;
    }

    nOut = nValue;
    return true;
}

static void WriteRowValue(const MenuRow& row, void* pControl, int nValue)
{
    if (!IsRowBound(row, pControl))
        return;

    auto ppVTable = *reinterpret_cast<uintptr_t**>(pControl);
    auto pSet = reinterpret_cast<SetRowValue_t>(ppVTable[nRowSetValueSlot]);

    if (row.nKind == ROW_BOOL || row.nKind == ROW_TICK)
    {
        // Matching is by value rather than by index: the control scans its byte array for one that
        // compares equal and selects that position. A value it does not hold is a silent no-op.
        uint8_t nByte = nValue != 0 ? 1 : 0;
        pSet(pControl, nullptr, &nByte);
    }
    else
    {
        pSet(pControl, nullptr, &nValue);
    }
}

// ------------------------------------------------------------------------------------------------
// Our page's identity.
//
// The engine's runtime type record is { const char* name, uint32_t depth, uint32_t nameHash[depth]
// } and a class is identified by the address of the trailing hash slot rather than by the hash.
// Ours is derived from the cloned class's record, so we inherit its ancestry and an is-a test
// against any base still succeeds.

static const char szJackalFixPageClass[] = "CFCXJackalFixOptionPage";

static uint32_t JackalFixClassRecord[24]{};
static const uint32_t* pJackalFixClassId = nullptr;
static const uint32_t* pClonedClassId = nullptr;

// Index one holds the vtable proper; index zero is the RTTI slot that sits just before it.
static uintptr_t JackalFixVTable[nVTableSlots + 1]{};

static void* pJackalFixPage = nullptr;

// The list widget's table, taken over per instance so only this page's list turns pages.
static uintptr_t JackalFixListVTable[nListBoxVTableSlots + 1]{};
static ListBoxNav_t StockListBoxNav = nullptr;
static ListBoxNav_t StockListBoxMouse = nullptr;
static ListBoxNav_t StockListBoxHover = nullptr;
static ListBoxNav_t StockListBoxPointer = nullptr;

// The class's own table, kept so a widget can be told apart from one already taken over, and so a
// widget of some other class is never handed a list's table by mistake.
static uintptr_t* pStockListVTable = nullptr;
static void* pJackalFixList = nullptr;

// ------------------------------------------------------------------------------------------------
// The overrides our clone carries. Everything else is stock behaviour.

static uint32_t* __fastcall JackalFixGetClassRecord(void*, void*)
{
    return JackalFixClassRecord;
}

// Captures whatever the user has moved on the page about to be torn down. Rows are destroyed on
// every page turn, clear-rows deleting the controls, the delegates and the map behind them, so a
// value that only lives in a control is a value that is about to be lost.
static void CapturePendingValues()
{
    for (size_t i = 0; i < nBuiltRows; i++)
    {
        const auto* pRow = Slots[BuiltRows[i].nSlot].pRow;
        if (pRow == nullptr)
            continue;

        int nValue = 0;
        if (ReadRowValue(*pRow, BuiltRows[i].pControl, nValue))
            PendingValues[BuiltRows[i].nSlot] = nValue;
    }
}

// What the rows on screen were built to believe about the three qualities.
static uint32_t nBuiltAvailability = 0;

static void ResetPendingValues()
{
    for (size_t i = 0; i < nSlots; i++)
    {
        const auto* pRow = Slots[i].pRow;
        PendingValues[i] = pRow != nullptr ? SettingToRow(*pRow) : 0;
    }
    bPendingValid = true;
}

static void BuildRows(void* pPage)
{
    nBuiltRows = 0;
    nTracedDraws = 0;
    nBuiltAvailability = BeyondUltraAvailability();

    // The row APIs look their widget template up inside the page's document and do not check that
    // there is one, so a missing document is a null dereference several frames down. Refuse rather
    // than crash: an empty page is a symptom, a crash is a puzzle.
    auto pDocument = *reinterpret_cast<void**>(static_cast<uint8_t*>(pPage) + nPageDocument);
    if (pDocument == nullptr)
    {
        return;
    }

    if (nCurrentPage >= nPages)
        nCurrentPage = 0;

    auto nFirst = nCurrentPage * nRowLines;

    for (size_t nLine = 0; nLine < nRowLines; nLine++)
    {
        auto nSlot = nFirst + nLine;
        const auto* pRow = nSlot < nSlots ? Slots[nSlot].pRow : nullptr;

        // A line only shows its arrows when something is actually bound to them. A heading has no
        // value and a blank has no row at all, and either drawn against a live < > reads as a
        // setting that has lost its value, so the widget goes off the page for this window and
        // comes back the moment a value row lands on the line again.
        auto bHasValue = pRow != nullptr && pRow->nKind != ROW_HEADING;
        MoveRowWidget(LineWidgets[nLine], bHasValue ? LineY[nLine] : nParkedY);

        // A blank keeps the labels level with the widgets they are paired to. Disabled, so the
        // navigation steps straight over it rather than parking on nothing.
        if (pRow == nullptr)
        {
            AddEntry(pPage, nullptr, L"", 0, nullptr);
            continue;
        }

        // A heading is a label and nothing else. It is added enabled, because the only way to tell
        // the engine a row cannot be selected is the same flag that greys it out, and a section
        // title drawn in the disabled ink reads as a setting that has been turned off. The
        // selection is kept off it in the navigation instead, where it costs nothing and looks
        // like nothing.
        if (pRow->nKind == ROW_HEADING)
        {
            AddEntry(pPage, nullptr, pRow->pLabel, 1, nullptr);
            continue;
        }

        const auto* pWidgetName = RowLines[nLine].pWidgetName;
        auto nValue = PendingValues[nSlot];
        void* pControl = nullptr;

        // Greying and unselectable are one flag, as the heading above says, which is exactly what
        // a row that can do nothing wants. Marking the item afterwards is not the same thing: the
        // ink is decided as the row is built.
        const auto bAvailable = IsRowAvailable(*pRow);
        const auto nEnabled = bAvailable ? 1 : 0;

        // No delegate on a value row: it is changed in place rather than opening a page. That is
        // also what greys out Accept while one is selected, exactly as on Game Options.
        switch (pRow->nKind)
        {
        case ROW_BOOL:
            pControl = AddValueRow(pPage, nullptr, pRow->pLabel, szRowTemplate, pWidgetName, L"On", L"Off", nEnabled, nullptr);
            break;

        case ROW_TICK:
            // The nearest thing to a tick this menu has. ASCII deliberately: the menu font is not
            // guaranteed to carry U+2713, and a box drawn as a missing glyph is worse than a
            // letter.
            pControl = AddValueRow(pPage, nullptr, pRow->pLabel, szRowTemplate, pWidgetName, L"[X]", L"[  ]", nEnabled, nullptr);
            break;

        case ROW_ENUM:
        {
            nValue = NearestEnumValue(*pRow, nValue);

            auto nCount = pRow->nValueCount;
            const int* pValues = pRow->pValues;
            const wchar_t* const* pLabels = pRow->pValueLabels;

            // A row that cannot do anything is built holding the one choice it already has. The
            // enabled flag greys it and takes it off the selection, but it does not stop a click
            // landing on an arrow, and the engine will happily walk a list it was handed, so it is
            // handed a list with nowhere to walk to. The value still reads, and moving lands on it.
            if (!bAvailable)
                nCount = CollapseToCurrent(nLine, nValue, pValues, pLabels, nCount);

            pControl = AddMultiValueRow(pPage, nullptr, pRow->pLabel, szRowTemplate, pWidgetName,
                                        nCount, pLabels, pValues, nEnabled, nullptr);
            break;
        }

        case ROW_RANGE:
        {
            auto nCount = BuildSteps(nLine, *pRow, nValue);
            const int* pValues = StepValues[nLine];
            const wchar_t* const* pLabels = StepLabels[nLine];

            if (!bAvailable)
                nCount = CollapseToCurrent(nLine, nValue, pValues, pLabels, nCount);

            pControl = AddMultiValueRow(pPage, nullptr, pRow->pLabel, szRowTemplate, pWidgetName,
                                        nCount, pLabels, pValues, nEnabled, nullptr);
            break;
        }

        case ROW_SLIDER:
            pControl = AddSliderRow(pPage, nullptr, pRow->pLabel, szRowTemplate, pWidgetName,
                                    pRow->nMinimum, pRow->nMaximum, nEnabled, nullptr);
            break;

        default:
            break;
        }

        if (pControl == nullptr)
        {
            continue;
        }

        PendingValues[nSlot] = nValue;
        BuiltRows[nBuiltRows++] = { nSlot, pControl };
        WriteRowValue(*pRow, pControl, nValue);

    }

    // Headings, having been added as ordinary rows so they draw in the ordinary ink, are now marked
    // unselectable, and their cached appearance is put back to a real row's, so the marking costs
    // nothing visually. Done to the items rather than through AddEntry, because AddEntry sets
    // both at once and there is no asking it for one without the other.
    if (auto ppItems = SafeRead<uint8_t**>(pPage != nullptr ? SafeRead<void*>(pPage, nPageList) : nullptr, nListBoxItems))
    {
        for (size_t nLine = 0; nLine < nRowLines && !bOrdinaryItemState; nLine++)
        {
            auto nSlot = nFirst + nLine;
            const auto* pRow = nSlot < nSlots ? Slots[nSlot].pRow : nullptr;

            // A row built greyed is not an ordinary one to copy from, and taking its appearance
            // would put every heading on the page into the disabled ink.
            if (pRow == nullptr || pRow->nKind == ROW_HEADING || !IsRowAvailable(*pRow))
                continue;

            if (auto pItem = ppItems[nLine])
            {
                nOrdinaryItemState = SafeRead<uint32_t>(pItem, nItemVisualState);
                bOrdinaryItemState = true;
            }
        }

        for (size_t nLine = 0; nLine < nRowLines; nLine++)
        {
            auto nSlot = nFirst + nLine;
            const auto* pRow = nSlot < nSlots ? Slots[nSlot].pRow : nullptr;
            if (pRow == nullptr)
                continue;

            if (pRow->nKind != ROW_HEADING)
                continue;

            auto pItem = ppItems[nLine];
            if (pItem == nullptr || !IsReadable(pItem, nItemSize))
                continue;

            *reinterpret_cast<uint8_t*>(pItem + nItemDisabled) = 1;
            if (bOrdinaryItemState)
                *reinterpret_cast<uint32_t*>(pItem + nItemVisualState) = nOrdinaryItemState;
        }
    }

}

// Whether anything on this page has been changed since it was opened. This is the option layer's
// own bookkeeping rather than ours: the per-row change handler lights APPLY only while it is
// clear, the APPLY handler refuses to run while it is clear, and the confirmation box behind
// DEFAULT clears it again on the way out.
static bool IsPageChanged(void* pPage)
{
    if (pPage == nullptr || nPageChanged == 0)
        return false;

    return *(static_cast<uint8_t*>(pPage) + nPageChanged) != 0;
}

static void SetPageChanged(void* pPage, bool bChanged)
{
    if (pPage == nullptr || nPageChanged == 0)
        return;

    *(static_cast<uint8_t*>(pPage) + nPageChanged) = bChanged ? uint8_t{ 1 } : uint8_t{ 0 };
}

// Lights or greys one of the prompts along the bottom of the page. One is APPLY and two is DEFAULT,
// which is the numbering the base open itself uses.
static void SetActionPrompt(void* pPage, int nPrompt, bool bEnabled)
{
    if (pPage == nullptr || DispatcherSlot == nullptr || SetPromptEnabled == nullptr || nEventDispatcher == 0)
        return;

    if (auto pSlot = DispatcherSlot(static_cast<uint8_t*>(pPage) + nEventDispatcher, nullptr, nPrompt))
        SetPromptEnabled(pSlot, nullptr, bEnabled ? 1 : 0);
}

// Tears the window down and puts the next one up. Clear-rows is thorough: it deletes every control,
// every delegate and every node of the row map, and empties the little value widgets each control
// had bound, so nothing accumulates across a page turn.
static void ShowPage(void* pPage, size_t nPage, bool bKeepOnScreenValues = true, int nTurnDirection = 0)
{
    if (nPage >= nPages)
        return;

    // Normally what is on screen is the newest truth and is taken before the rows are destroyed.
    // Not when the caller has just decided what every row should say. DEFAULT does exactly that,
    // and reading the old controls back over it is why pressing it appeared to do nothing.
    if (bKeepOnScreenValues)
        CapturePendingValues();

    auto nPrevious = nCurrentPage;
    auto bSameWindow = nPage == nCurrentPage;
    auto pList = SafeRead<void*>(pPage, nPageList);

    // Where the user was standing. A rebuild that is not a page turn, whether DEFAULT or the ini
    // changing underneath, should put them back on the same line rather than at one end.
    auto nWasOn = SafeRead<int32_t>(pList, nListBoxSelected);

    // Everything the option layer knows about this page is about to be destroyed with the rows, so
    // the one bit worth keeping is taken first.
    auto bChanged = IsPageChanged(pPage);

    auto ppVTable = *reinterpret_cast<uintptr_t**>(pPage);
    reinterpret_cast<PageMethod_t>(ppVTable[nClearRowsSlot])(pPage, nullptr);

    nCurrentPage = nPage;
    BuildRows(pPage);

    // Clearing the rows destroyed the controls, and with them the option layer's subscription to
    // every row's change event, which is the only thing that ever lights APPLY. Those
    // subscriptions are planted by the base open, so it is run again. It also declares the page
    // unchanged and greys APPLY on the way through, which is right for a page being opened and
    // wrong for one being rebuilt underneath somebody, so that much is put back.
    if (BasePageOpen != nullptr)
    {
        BasePageOpen(pPage, nullptr);

        if (bChanged)
        {
            SetPageChanged(pPage, true);
            SetActionPrompt(pPage, nPromptApply, true);
            SetActionPrompt(pPage, nPromptDefault, true);
        }
    }

    // Coming down onto a page starts at the top; coming up onto one starts at the bottom, which is
    // what the eye expects when the page turns underneath a held direction. A rebuild of the window
    // already on screen is not a turn, so it resumes where it left off.
    //
    // Which way the page moved is normally the page order, but not when the list has wrapped: the
    // last page turning down onto the first is a step down even though the number went backwards,
    // and landing at the bottom of it would read as having gone the wrong way. So the caller says
    // so where it knows, and the order answers where it does not.
    auto bDownwards = nTurnDirection != 0 ? nTurnDirection > 0 : nPage > nPrevious;
    auto bResume = bSameWindow && nWasOn >= 0 && nWasOn < static_cast<int>(nRowLines);
    auto nFrom = bResume ? nWasOn : (bDownwards ? 0 : static_cast<int>(nRowLines) - 1);

    pList = SafeRead<void*>(pPage, nPageList);
    SettleSelection(pList, nFrom, bResume || bDownwards ? 1 : -1);

    // The lit bar is a separate field from the selection and does not follow it, so on a rebuild in
    // place it is moved by hand, or the light is left on whatever line the old rows had it.
    if (bResume && SetHighlight != nullptr && pRowListFocusable != nullptr && pList != nullptr)
    {
        auto nNow = SafeRead<int32_t>(pList, nListBoxSelected);
        if (nNow >= 0)
            SetHighlight(pList, nullptr, pRowListFocusable, nInputUser, nNow, nHighlightKeyboard);
    }

    // Clearing the list left the selection empty, and the first row added takes it, so the
    // selection lands on the top line of the new page in both directions. Deliberate: pressing up
    // again from there turns another page, which is the behaviour a paged list wants.
    if (pList != nullptr)
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pList) + nListBoxFirstVisible) = 0;
}

// On drawing section titles and settings in different inks: not done, and here is the state of it
// so it is not attempted again from scratch.
//
// The rows share one template, so there is no per-row colour to write. Two routes were tried and
// both were wrong in a way that took the game down:
//
//   - The engine's own per-row hook, the index array at ListBox+DCh, only chooses between the
//     template's authored states. State four on this template is the red highlight, so a marked
//     heading is painted as though it were selected. That one is a dead end on the artwork rather
//     than on the code.
//   - Setting the template's colour from inside the row draw. The idea is sound, since the list
//     re-poses the one shared template immediately before each row and the colour only has to be
//     right for that instant, but it needs the template's State, and that was guessed at twice and
//     wrong twice. ListBox+3Ch is a widget; taking +14h off it produced a pointer into the DLL's
//     read-only data, and reading +08h off the widget produced its name CRC, which is a hash
//     rather than a pointer at all.
//
// What is missing is one verified fact: how to get from the row template to the State that carries
// its colour. Until that is established from the disassembly rather than inferred, this stays out;
// a cosmetic tint is not worth another crash.

// The four draws the fade runs off.
//
// Both of magma::Text's string routines and magma::Image's draw write the row's colour first; which
// row that is comes from the counter, which the list's own draw resets and the per-cell draw steps.
// Nothing here decides anything: the line is looked up, and a line that is fine is left alone.
static void __fastcall JackalFixTextDraw(void* pText, void* pEdx, void* pArg1, void* pArg2)
{
    const auto faded = FadeForDraw(pText, nTextVtableRva, nTextStateColour, 1);
    TextDrawHook.fastcall(pText, pEdx, pArg1, pArg2);
    RestoreAfterDraw(faded);
}

static void __fastcall JackalFixTextDrawPlain(void* pText, void* pEdx, void* pArg)
{
    const auto faded = FadeForDraw(pText, nTextVtableRva, nTextStateColour, 1);
    TextDrawPlainHook.fastcall(pText, pEdx, pArg);
    RestoreAfterDraw(faded);
}

static void __fastcall JackalFixImageDraw(void* pImage, void* pEdx)
{
    const auto faded = FadeForDraw(pImage, nImageVtableRva, nImageStateColour, nImageStateColourCount);
    ImageDrawHook.fastcall(pImage, pEdx);
    RestoreAfterDraw(faded);
}

// Which line a widget is the value of, for the case where a value list is drawn in its own right
// rather than from inside the row it sits on.
static int LineForWidget(const void* pWidget)
{
    if (pWidget == nullptr)
        return -1;

    for (size_t i = 0; i < nRowLines; i++)
        if (LineWidgets[i] == pWidget)
            return static_cast<int>(i);

    return -1;
}

// Held for the length of one row's draw and put back rather than cleared. A list that is not ours
// does not clear it: our own values are lists too, and clearing is what left them black.
static void __fastcall JackalFixDrawRow(void* pList, void* pEdx, int nIndex, void* pPosition)
{
    const auto nPrevious = nDrawingLine;

    if (pList == pJackalFixList)
        nDrawingLine = nIndex;

    DrawRowHook.fastcall(pList, pEdx, nIndex, pPosition);
    nDrawingLine = nPrevious;
}

/*
  The other way in, and the one that catches what the rows themselves cannot.

  A value list caught here is on a line whether or not the row it belongs to is what is drawing it.
  The row list is on one too, and that is the piece that was missing: CListBox::Draw walks its cells
  between two other widgets of its own, ListBox+58h before them and ListBox+74h after. The second is
  the highlight, the thing that draws the row under the pointer over the top of what the row
  already drew. It is outside the cell loop, so the per-row hook never sees it, and it is drawn at
  full strength, which is why an arrow that had already been faded came back black the moment the
  pointer touched it.

  It belongs to whichever line the light is on, and the list says which at ListBox+D0h. So the whole
  of the row list's draw starts out attributed to that line; the per-row hook overrides it for each
  cell as it goes past and puts it back, which leaves exactly the two outside draws carrying it.
*/
static void __fastcall JackalFixListDraw(void* pList, void* pEdx)
{
    const auto nPrevious = nDrawingLine;

    if (const auto nLine = pList != pJackalFixList ? LineForWidget(pList) : -1; nLine >= 0)
        nDrawingLine = nLine;

    ListDrawHook.fastcall(pList, pEdx);
    nDrawingLine = nPrevious;
}

// Puts a heading's appearance back to an ordinary row's after the engine has recomputed it. The
// disabled flag stays set, which is what keeps the selection off it, and only the look is undone.
static void __fastcall JackalFixRefreshRow(void* pListBox, void* pEdx, int nIndex, int nAnimate)
{
    RefreshRowHook.fastcall(pListBox, pEdx, nIndex, nAnimate);

    if (pListBox != pJackalFixList || nIndex < 0 || nIndex >= static_cast<int>(nRowLines))
        return;

    auto nSlot = nCurrentPage * nRowLines + static_cast<size_t>(nIndex);
    const auto* pRow = nSlot < nSlots ? Slots[nSlot].pRow : nullptr;
    if (pRow == nullptr)
        return;

    // Nothing is done to an ordinary row here any more. The refresh is what puts a row's colours
    // back where the artwork wants them, and a value coloured against that is undone the moment the
    // row redraws, so the fade waits for the draw itself instead of racing this.
    if (pRow->nKind != ROW_HEADING)
        return;

    if (!bOrdinaryItemState)
        return;

    auto ppItems = SafeRead<uint8_t**>(pListBox, nListBoxItems);
    auto pItem = ppItems != nullptr ? ppItems[nIndex] : nullptr;
    if (pItem != nullptr && IsReadable(pItem, nItemSize))
        *reinterpret_cast<uint32_t*>(pItem + nItemVisualState) = nOrdinaryItemState;
}

// A line the selection is allowed to rest on: something with a value behind it, rather than a
// section title or a spacer.
static bool IsSelectableLine(size_t nLine)
{
    auto nSlot = nCurrentPage * nRowLines + nLine;
    const auto* pRow = nSlot < nSlots ? Slots[nSlot].pRow : nullptr;
    return pRow != nullptr && pRow->nKind != ROW_HEADING && IsRowAvailable(*pRow);
}

// The page the list moves to when the selection walks off one end of the window.
//
// The ends join. A single page has nowhere to go and says so by answering itself, which is what
// both callers already test for; past that, down from the last page is the first and up from the
// first is the last. The rows inside a window still step one at a time; this is only what happens
// once there is no next row in the window to step onto.
static size_t NextPage(int nDirection)
{
    if (nPages <= 1)
        return nCurrentPage;

    if (nDirection > 0)
        return nCurrentPage + 1 < nPages ? nCurrentPage + 1 : 0;

    return nCurrentPage > 0 ? nCurrentPage - 1 : nPages - 1;
}

// Puts the selection on the nearest line worth resting on, preferring the direction of travel and
// falling back to the other way, which is what the top of the first page needs, where a heading has
// nothing above it to step onto.
static void SettleSelection(void* pListBox, int nFrom, int nDirection)
{
    if (SetSelection == nullptr || pListBox == nullptr || nRowLines == 0)
        return;

    if (nDirection == 0)
        nDirection = 1;

    for (int nPass = 0; nPass < 2; nPass++)
    {
        auto nStep = nPass == 0 ? nDirection : -nDirection;
        for (auto i = nFrom; i >= 0 && i < static_cast<int>(nRowLines); i += nStep)
        {
            if (!IsSelectableLine(static_cast<size_t>(i)))
                continue;

            SetSelection(pListBox, nullptr, i, 1, 1);
            return;
        }
    }
}

// Moves the selection one row, turning the page when it runs off the end. This is done explicitly
// rather than by handing the key to the stock handler: with a value focused, that handler was
// reached, detected the key and did nothing at all. It acts on the widget the input was routed to,
// and routing it elsewhere is not something a caller can ask for.
static bool StepRow(void* pPage, int nDirection)
{
    if (pJackalFixList == nullptr || SetSelection == nullptr || nRowLines == 0)
        return false;

    auto nSelected = SafeRead<int32_t>(pJackalFixList, nListBoxSelected);

    // A selection outside the window is not a reason to turn the page. It means the list is not
    // sitting where this thinks it is, and stepping should still land inside. Start from just
    // outside the near end so the first step comes in.
    auto nFrom = nSelected;
    if (nFrom < 0 || nFrom >= static_cast<int>(nRowLines))
        nFrom = nDirection > 0 ? -1 : static_cast<int>(nRowLines);

    auto nNext = nFrom + nDirection;
    while (nNext >= 0 && nNext < static_cast<int>(nRowLines) && !IsSelectableLine(static_cast<size_t>(nNext)))
        nNext += nDirection;

    if (nNext >= 0 && nNext < static_cast<int>(nRowLines))
    {
        SetSelection(pJackalFixList, nullptr, nNext, 1, 1);

        // And light it, which the selection alone does not do.
        if (SetHighlight != nullptr && pRowListFocusable != nullptr)
            SetHighlight(pJackalFixList, nullptr, pRowListFocusable, nInputUser, nNext, nHighlightKeyboard);

        return true;
    }

    // Off the end of the window, so the window moves instead, and off the end of the last window
    // is the first one, which is what a list of pages with no beginning or end means.
    auto nPage = NextPage(nDirection);
    if (nPage == nCurrentPage || pPage == nullptr)
        return false;

    ShowPage(pPage, nPage, true, nDirection);
    return true;
}

// The list's own input handler, taken over for this page's list alone.
//
// The engine's version tries to move the selection and, when it cannot, records the direction it
// could not move in and asks for the input to be handed on. That refusal is the page turn: wrapping
// is switched off precisely so the refusal happens rather than the selection quietly jumping back
// to the top of the same window.
//
// Rebuilding from here is safe. The handler caches no item pointer across the call it just made,
// re-reading the selection, the first visible index and the visible count afterwards, and the up
// and down path never runs through the row delegates that clear-rows deletes.
// Which line a value belongs to, or -1 for anything that is not one of this page's values.
static int ValueWidgetLine(void* pWidget)
{
    for (size_t i = 0; i < nRowLines; i++)
    {
        if (LineWidgets[i] == pWidget && pWidget != nullptr)
            return static_cast<int>(i);
    }
    return -1;
}

static bool IsValueWidget(void* pWidget)
{
    return ValueWidgetLine(pWidget) >= 0;
}

// Clicking a value leaves the keyboard pointed at that value, and it only understands left and
// right, so nothing moved afterwards until the left of the page was clicked, which put the
// selection back on the rows. That is exactly what this does, without the second click: once the
// click has been dealt with, the row it belongs to is selected again.
// The pointer can put the light on a section title just by passing over it. The highlight is a
// different field from the selection, so none of the heading skipping in the navigation applies to
// it. Whichever way the pointer arrives, the light is moved on to the nearest real row.
static void KeepLightOffHeadings(void* pListBox, void* pSender, void* pEvent)
{
    if (pListBox != pJackalFixList || SetHighlight == nullptr || pSender == nullptr)
        return;

    auto nLit = SafeRead<int32_t>(pListBox, nListBoxHighlight);
    if (nLit < 0 || nLit >= static_cast<int>(nRowLines) || IsSelectableLine(static_cast<size_t>(nLit)))
        return;

    auto nNext = nLit;
    while (nNext < static_cast<int>(nRowLines) && !IsSelectableLine(static_cast<size_t>(nNext)))
        nNext++;

    if (nNext >= static_cast<int>(nRowLines))
    {
        nNext = nLit;
        while (nNext >= 0 && !IsSelectableLine(static_cast<size_t>(nNext)))
            nNext--;
    }

    if (nNext < 0 || nNext >= static_cast<int>(nRowLines))
        return;

    SetHighlight(pListBox, nullptr, pSender, SafeRead<uint8_t>(pEvent, 8), nNext, nHighlightPointer);
    if (SetSelection != nullptr)
        SetSelection(pListBox, nullptr, nNext, 1, 1);
}

// A value belonging to a row that cannot be used takes no pointer at all. The click never reaches
// the arrow underneath it and the value never takes the focus. The keyboard was already kept away
// by the row not being selectable.
static bool IsPointerRefused(void* pListBox)
{
    const auto nLine = ValueWidgetLine(pListBox);
    return nLine >= 0 && IsLineUnavailable(nLine);
}

static int __fastcall JackalFixListBoxHover(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult)
{
    if (IsPointerRefused(pListBox))
        return 0;

    if (pListBox == pJackalFixList && pSender != nullptr)
        pRowListFocusable = pSender;

    auto nReturn = StockListBoxHover(pListBox, pEdx, pSender, pEvent, pResult);
    KeepLightOffHeadings(pListBox, pSender, pEvent);
    return nReturn;
}

static int __fastcall JackalFixListBoxPointer(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult)
{
    if (IsPointerRefused(pListBox))
        return 0;

    if (pListBox == pJackalFixList && pSender != nullptr)
        pRowListFocusable = pSender;

    auto nReturn = StockListBoxPointer(pListBox, pEdx, pSender, pEvent, pResult);
    KeepLightOffHeadings(pListBox, pSender, pEvent);
    return nReturn;
}

static int __fastcall JackalFixListBoxMouse(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult)
{
    if (IsPointerRefused(pListBox))
        return 0;

    if (pListBox == pJackalFixList && pSender != nullptr)
        pRowListFocusable = pSender;

    auto nReturn = StockListBoxMouse(pListBox, pEdx, pSender, pEvent, pResult);

    KeepLightOffHeadings(pListBox, pSender, pEvent);

    auto nLine = ValueWidgetLine(pListBox);
    if (nLine >= 0)
    {
        // Nothing is taken away here. A value needs the light to be worked with the mouse, and
        // taking it back on the click is what stopped the arrows changing anything. The row only
        // takes it back when a key arrives that the value cannot use; see the input handler.
        nInputUser = SafeRead<uint8_t>(pEvent, 8);
        pValueFocusable = pSender;
        nValueLine = nLine;

        if (SetSelection != nullptr && pJackalFixList != nullptr)
            SetSelection(pJackalFixList, nullptr, nLine, 1, 1);
    }

    return nReturn;
}

static int __fastcall JackalFixListBoxNav(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult)
{
    if (pListBox == pJackalFixList && pSender != nullptr)
        pRowListFocusable = pSender;
    nInputUser = SafeRead<uint8_t>(pEvent, 6);

    RefreshIfStale(pJackalFixPage);

    auto nFlagsBefore = pResult != nullptr ? pResult[nNavResultFlags] : 0;
    auto nReturn = StockListBoxNav(pListBox, pEdx, pSender, pEvent, pResult);

    // Clicking a value hands the keyboard to that value's own little list, and it only knows left
    // and right, so up and down went nowhere and the page appeared to seize until something on the
    // left was clicked to give the focus back. A value list that refuses an up or a down passes it
    // to the list of rows, which is where it was always meant to go.
    if (pListBox != pJackalFixList && pResult != nullptr && IsValueWidget(pListBox))
    {
        auto bRefusedHere = (pResult[nNavResultFlags] & nNavFlagUnhandled) != 0
                         && (nFlagsBefore & nNavFlagUnhandled) == 0;
        auto nCode = *reinterpret_cast<int32_t*>(pResult + nNavResultCode);

        if (!bRefusedHere || (nCode != nNavCodeUp && nCode != nNavCodeDown))
            return nReturn;

        // An up or a down is not something a value can use, so this is where the light goes back to
        // the rows, after the click has had its use of it rather than instead of it.
        if (SetHighlight != nullptr && pSender != nullptr)
        {
            SetHighlight(pListBox, nullptr, pSender, nInputUser, -1, nHighlightPointer);
            SetHighlight(pListBox, nullptr, pSender, nInputUser, -1, nHighlightKeyboard);
        }
        pValueFocusable = nullptr;

        // The row is moved here rather than by passing the key on. Swallow the refusal either way,
        // or the focus is handed off to whatever sits beyond the page.
        StepRow(pJackalFixPage, nCode == nNavCodeDown ? 1 : -1);
        pResult[nNavResultFlags] = nFlagsBefore;
        return 1;
    }

    // The table is swapped per instance, so this should only ever be our own list, but the check
    // costs a compare and the failure mode it guards against is every list in the game turning
    // pages.
    if (pListBox != pJackalFixList || pResult == nullptr || pJackalFixPage == nullptr || nPages <= 1)
        return nReturn;

    // Headings are ordinary selectable rows as far as the engine is concerned, so landing on one
    // is stepped over by moving again the same way. Bounded by the number of lines, because a
    // window of nothing but headings would otherwise spin here.
    for (size_t nGuard = 0; nGuard < nRowLines; nGuard++)
    {
        if ((pResult[nNavResultFlags] & nNavFlagUnhandled) != 0)
            break;

        auto nSelected = SafeRead<int32_t>(pListBox, nListBoxSelected);
        if (nSelected < 0)
            break;

        // Headings and rows the game's own settings have made pointless are both landed on by the
        // engine and both stepped over here, for the same reason and in the same way.
        if (IsSelectableLine(static_cast<size_t>(nSelected)))
            break;

        nReturn = StockListBoxNav(pListBox, pEdx, pSender, pEvent, pResult);
    }

    // Stepping over only works while there is somewhere to step. At the ends of a page there is
    // not, so the selection is placed outright.
    KeepLightOffHeadings(pListBox, pSender, pEvent);

    auto nResting = SafeRead<int32_t>(pListBox, nListBoxSelected);
    if (nResting >= 0 && !IsSelectableLine(static_cast<size_t>(nResting)))
    {
        auto nCode = *reinterpret_cast<int32_t*>(pResult + nNavResultCode);
        SettleSelection(pListBox, nResting, nCode == nNavCodeUp ? -1 : 1);
    }

    auto bRefused = (pResult[nNavResultFlags] & nNavFlagUnhandled) != 0
                 && (nFlagsBefore & nNavFlagUnhandled) == 0;
    if (!bRefused)
        return nReturn;

    auto nCode = *reinterpret_cast<int32_t*>(pResult + nNavResultCode);
    if (nCode != nNavCodeUp && nCode != nNavCodeDown)
        return nReturn;

    auto nDirection = nCode == nNavCodeDown ? 1 : -1;
    auto nNext = NextPage(nDirection);
    if (nNext == nCurrentPage)
        return nReturn;

    ShowPage(pJackalFixPage, nNext, true, nDirection);

    // Keep the focus on the list. Without this the refusal we acted on would also hand the key to
    // whatever sits above or below the list, and the selection would leave the page entirely.
    pResult[nNavResultFlags] &= static_cast<uint8_t>(~nNavFlagUnhandled);
    return 1;
}

// Swapped per instance rather than hooked, because the class is shared by every list in the game
// and only this one should turn pages. Repeated on every open: the document outlives a menu
// session, but so would a stale table if the engine ever rebuilt the widget.
static void TakeOverList(void* pPage)
{
    auto pList = *reinterpret_cast<void**>(static_cast<uint8_t*>(pPage) + nPageList);
    if (pList == nullptr)
    {
        return;
    }

    auto ppVTable = *reinterpret_cast<uintptr_t**>(pList);

    if (ppVTable != &JackalFixListVTable[1])
    {
        auto pSource = ppVTable - 1; // the RTTI pointer sits one slot before the table
        for (size_t i = 0; i <= nListBoxVTableSlots; i++)
            JackalFixListVTable[i] = pSource[i];

        pStockListVTable = ppVTable;
        StockListBoxNav = reinterpret_cast<ListBoxNav_t>(JackalFixListVTable[1 + nListBoxNavSlot]);
        JackalFixListVTable[1 + nListBoxNavSlot] = reinterpret_cast<uintptr_t>(JackalFixListBoxNav);

        StockListBoxMouse = reinterpret_cast<ListBoxNav_t>(JackalFixListVTable[1 + nListBoxMouseSlot]);
        JackalFixListVTable[1 + nListBoxMouseSlot] = reinterpret_cast<uintptr_t>(JackalFixListBoxMouse);

        StockListBoxHover = reinterpret_cast<ListBoxNav_t>(JackalFixListVTable[1 + nListBoxHoverSlot]);
        JackalFixListVTable[1 + nListBoxHoverSlot] = reinterpret_cast<uintptr_t>(JackalFixListBoxHover);

        StockListBoxPointer = reinterpret_cast<ListBoxNav_t>(JackalFixListVTable[1 + nListBoxPointerSlot]);
        JackalFixListVTable[1 + nListBoxPointerSlot] = reinterpret_cast<uintptr_t>(JackalFixListBoxPointer);

        *reinterpret_cast<uintptr_t**>(pList) = &JackalFixListVTable[1];
    }

    pJackalFixList = pList;

    // The value lists get the same table. They are this page's own widgets, so nothing else in the
    // game is affected, and it is the only way an up or a down pressed while one of them holds the
    // focus can be handed back to the rows.
    size_t nTaken = 0;
    for (size_t i = 0; i < nRowLines; i++)
    {
        auto pWidget = LineWidgets[i];
        if (pWidget == nullptr || PointsIntoDunia(pWidget))
            continue;

        // Only a widget still carrying the stock table, and only if that is what it is. Anything
        // of another class is left alone rather than being handed a list's methods.
        if (pStockListVTable != nullptr && *reinterpret_cast<uintptr_t**>(pWidget) == pStockListVTable)
        {
            *reinterpret_cast<uintptr_t**>(pWidget) = &JackalFixListVTable[1];
            nTaken++;
        }
        else if (*reinterpret_cast<uintptr_t**>(pWidget) == &JackalFixListVTable[1])
        {
            nTaken++;
        }
    }

    // One line per widget the layout carries, and no wrapping, both so the window matches the
    // furniture exactly and so a press past the last row is reported rather than swallowed.
    *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(pList) + nListBoxMaxVisible) = static_cast<uint8_t>(nRowLines);

    auto& nFlags = *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(pList) + nListBoxFlags);
    nFlags &= static_cast<uint8_t>(~nListBoxWrap);
}

// The page's own value-changed hook, raised on every arrow press with the row index that moved.
// CFCXOptionGamePage's version couples two of its rows together, so that picking a difficulty
// makes the machete row appear or hide, and does it by looking up row indices this clone never
// set. Nothing on this page depends on another row, so doing nothing is the correct behaviour and
// not merely a safe one.
//
// The engine still updates the control's own value before raising this, and the option layer's own
// subscriber still lights the Apply prompt, so the row moves and the page notices.
static void __fastcall JackalFixValueChanged(void*, void*, int)
{
}

// Apply. The action buttons are handled by the option-page layer, which dispatches here.
//
// Every row is written rather than only the ones on screen, because a page turn destroys the
// controls that held the earlier choices; they live in PendingValues instead. A row is only
// written when it actually differs from the setting, and the comparison is against the setting
// rather than against the row's own starting value, so nothing is rewritten by rounding: a
// FieldOfView of 91.31 reads back as 91 and would otherwise be flattened on every Apply.
//
// Writing the ini is what makes a change take effect: the plugin already watches that file and
// re-reads it, so the existing onIniFileChange path applies everything. It is fired here as well so
// the result does not depend on the watcher's timing, and because the watcher is only installed
// when the ini existed at start-up.
// True while this page is the one doing the writing, so the reaction to the ini changing can tell
// its own work from somebody else's and not throw away what the user is in the middle of.
static bool bApplyingOurselves = false;

// Set when the ini has moved under us and the rows on screen no longer match it. Acted on from the
// engine's thread, never from the watcher's.
static bool bRowsStale = false;

// Redraws the page if something changed it from outside. Only ever called from somewhere the engine
// itself called us.
static void RefreshIfStale(void* pPage)
{
    if (!bRowsStale)
        return;

    bRowsStale = false;

    if (pPage != nullptr && nBuiltRows != 0)
    {
        // Without the capture. The values were taken from the file the moment it changed, and
        // reading the controls back over them would put the rows on screen straight back to what
        // they said before the edit, which is the one thing this exists to undo.
        ShowPage(pPage, nCurrentPage, false);
    }
}

// Writes every staged value that differs from the setting and tells the rest of the plugin. Shared
// by APPLY and by DEFAULT, which both mean "this is now the truth" and differ only in where the
// staged values came from.
static size_t CommitPendingValues()
{
    CIniReader iniReader("");
    size_t nWritten = 0;
    bApplyingOurselves = true;

    for (size_t i = 0; i < nSlots; i++)
    {
        const auto* pRow = Slots[i].pRow;
        if (pRow == nullptr || pRow->nKind == ROW_HEADING || pRow->nPref == Pref::COUNT)
            continue;

        auto nValue = PendingValues[i];
        if (nValue == SettingToRow(*pRow))
            continue;

        const auto bHold = IsSessionOnly(pRow->nPref);

        if (pRow->nValue == VALUE_FLOAT)
        {
            auto fValue = static_cast<float>(nValue) / pRow->fScale;
            if (bHold)
            {
                JackalFixSettings.HoldFloat(pRow->nPref, fValue);
            }
            else
            {
                JackalFixSettings.SetFloat(pRow->nPref, fValue);
                iniReader.WriteFloat(pRow->pIniSection, pRow->pIniKey, fValue);
            }
        }
        else
        {
            if (bHold)
            {
                JackalFixSettings.HoldInt(pRow->nPref, nValue);
            }
            else
            {
                JackalFixSettings.SetInt(pRow->nPref, nValue);
                iniReader.WriteInteger(pRow->pIniSection, pRow->pIniKey, nValue);
            }
        }

        // A paired row carries a second key whose value is chosen by the same index.
        if (pRow->nPref2 != Pref::COUNT && pRow->pValues2 != nullptr)
        {
            for (uint32_t n = 0; n < pRow->nValueCount; n++)
            {
                if (pRow->pValues[n] != nValue)
                    continue;

                if (IsSessionOnly(pRow->nPref2))
                {
                    JackalFixSettings.HoldInt(pRow->nPref2, pRow->pValues2[n]);
                }
                else
                {
                    JackalFixSettings.SetInt(pRow->nPref2, pRow->pValues2[n]);
                    iniReader.WriteInteger(pRow->pIniSection, pRow->pIniKey2, pRow->pValues2[n]);
                }

                break;
            }
        }

        nWritten++;
    }

    // Fired whether or not anything moved. Everything that reacts to a setting hangs off this, so
    // firing it is what makes a change take hold in the running game rather than at the next start,
    // and a page that writes nothing has cost one broadcast.
    JackalFix::onIniFileChange().executeAll();
    bApplyingOurselves = false;
    return nWritten;
}

static void __fastcall JackalFixApplyPage(void*, void*)
{
    CapturePendingValues();
    CommitPendingValues();
}

// Revert, which the DEFAULT prompt reaches through the option-page layer's confirmation box. The
// stock page reloads the live configuration into every row; ours does the same from the settings
// store, which is the ini as last read, and then redraws the window it is on so the change shows.
// DEFAULT puts every row back to the value the mod ships with, on every page rather than only the
// one on screen, writes them, and shows the result.
//
// Writing here rather than leaving it staged for APPLY is not a shortcut. The option layer greys
// both prompts and declares the page unchanged the instant this returns, because the stock page's
// DEFAULT commits to the live configuration on the spot, so anything this left staged could never
// be committed at all, which is exactly what "pressing default does nothing" looked like.
static void __fastcall JackalFixDefaultPage(void* pPage, void*)
{
    size_t nRestored = 0;
    for (size_t i = 0; i < nSlots; i++)
    {
        const auto* pRow = Slots[i].pRow;
        if (pRow == nullptr || pRow->nKind == ROW_HEADING || pRow->nPref == Pref::COUNT)
            continue;

        PendingValues[i] = DefaultForRow(*pRow);
        nRestored++;
    }

    bPendingValid = true;
    auto nWritten = CommitPendingValues();

    // Without the capture, or the rows would be read back over the values just decided.
    if (pPage != nullptr)
        ShowPage(pPage, nCurrentPage, false);

}

// Events one, two and three are Apply, Revert and Back. The stock handler vtables are used as they
// are: each is a three instruction thunk onto the option-page layer's generic handler, and those
// dispatch through the page's own Apply and Revert slots, both of which the clone now owns. What
// made them unsafe before was CFCXOptionGamePage's versions of those slots rather than the
// handlers.
static void RegisterActionHandlers(void* pPage)
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

// Stands in for CFCXOptionGamePage::Open, which builds the stock game rows, clears a field and tail
// calls the base. Ours does the same with its own rows, so everything the base open expects to be
// set up still is, including the base's subscription of the option layer's change handler to every
// row in the map, which is what lights the Apply prompt when a value moves.
static void __fastcall JackalFixOpenPage(void* pPage, void* pEdx)
{
    auto pBytes = static_cast<uint8_t*>(pPage);
    auto ppVTable = *reinterpret_cast<uintptr_t**>(pPage);

    // CListMenuPage::Open makes the same test before it touches anything. The base open does not,
    // and walking its control map on a page the engine has not brought up yet is a fault, so the
    // guard belongs here too. Doing nothing leaves an empty page; not doing it crashes.
    if (*(pBytes + nPageReady) == 0 || *reinterpret_cast<void**>(pBytes + nPageDocument) == nullptr)
    {
        return;
    }

    // The line table, the moves and the clones all have to be settled before the slot plan is built
    // against them: the plan is a function of how many lines there are and what each one can hold.
    PrepareLayout(pPage);

    // Measured every time the page opens rather than once, because the window it is measuring is
    // one of the things this page can change.
    SettleRenderResolutionChoices();

    PlanSlots();

    // Said once per opening, and in this log rather than only the other one: a Beyond Ultra row
    // that will not grey out is either a quality that really does read as ultra or a profile that
    // was never found, and from the row alone the two look the same.
    {
        const auto* pGeometry = JackalFixGeometryQuality();
        const auto* pShadow = JackalFixShadowQuality();
        const auto* pTerrain = JackalFixTerrainQuality();

    }

    JackalFixTraceQualities();

    if (!bPendingValid)
        ResetPendingValues();

    // Before any row exists, exactly as the stock builder does it.
    RegisterActionHandlers(pPage);
    TakeOverList(pPage);

    nCurrentPage = 0;

    reinterpret_cast<PageMethod_t>(ppVTable[nClearRowsSlot])(pPage, pEdx);
    BuildRows(pPage);

    *reinterpret_cast<uint32_t*>(pBytes + nPageResetField) = 0;
    BasePageOpen(pPage, pEdx);

    // The Beyond Ultra rows are built against an answer that is not settled yet on the first
    // opening of a session. The log has it in two lines, "ultrahigh, ultrahigh, ultrahigh" on the
    // way in and the real qualities a moment later, because the profile the engine hands out
    // before the base open has run is a preset template, and the live one only appears once it
    // has. So the question is asked again on the way out and the rows are rebuilt if the answer
    // moved. One extra build, once, and never again in that session.
    if (BeyondUltraAvailability() != nBuiltAvailability)
    {
        ShowPage(pPage, nCurrentPage, false);
    }

    // After the base open rather than before it. The base takes the page over as it enters, putting
    // the selection on the first row among other things, so settling beforehand is undone a moment
    // later, which is why the page opened on a section title while stepping onto one during
    // navigation was handled correctly.
    SettleSelection(SafeRead<void*>(pPage, nPageList), 0, 1);
    bRowsStale = false;
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
        return false;
    }

    for (size_t i = 0; i < nSites; i++)
        injector::WriteMemory<uint32_t>(pSites[i], reinterpret_cast<uint32_t>(szJackalFixPageLayout), true);

    PageConstruct(pPage);

    for (size_t i = 0; i < nSites; i++)
        injector::WriteMemory<uint32_t>(pSites[i], nOriginals[i], true);

    return true;
}

// The Game row's delegate on the OPTIONS list carries CFCXOptionGamePage's class id, which is also
// what its page creator pushes, so one immediate out of BuildEntries identifies both the class we
// clone and the creator that tells us how to build it.
// Finding the creator only needs the address of the class id, which is a link-time constant, so
// this can run at start-up. Deriving our class record from the engine's cannot: that record is
// filled in lazily by the creator itself, and at plugin init it is still all zeros.
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
        break;
    }

    if (PageConstruct == nullptr)
    {
        return false;
    }

    return true;
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

// Runs once the engine has filled the record in, which the creator does on its way past. Everything
// here needs the real contents, so none of it can happen at start-up.
static bool ResolveClassRecord()
{
    if (pJackalFixClassId != nullptr)
        return true;

    auto pRecord = FindClassRecord(pClonedClassId);
    if (pRecord == nullptr)
    {
        return false;
    }

    // Same shape as the cloned class's record, with one more link on the chain for us. The engine
    // would put a hash of the class name in that last slot; anything unique does just as well,
    // since only the registry reads it. The address of our own name string is unique by
    // construction and can never be the 0xFFFFFFFF that marks a row as having no page behind it.
    JackalFixClassRecord[0] = reinterpret_cast<uint32_t>(szJackalFixPageClass);
    JackalFixClassRecord[1] = pRecord[1] + 1;
    for (uint32_t i = 0; i < pRecord[1]; i++)
        JackalFixClassRecord[2 + i] = pRecord[2 + i];
    JackalFixClassRecord[pRecord[1] + 2] = reinterpret_cast<uint32_t>(szJackalFixPageClass);

    pJackalFixClassId = &JackalFixClassRecord[JackalFixClassRecord[1] + 1];
    return true;
}

// True when the manager already has our page. The registry is rebuilt along with the menu, so this
// is asked every time rather than cached, since a page registered against a previous manager is
// gone.
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

        // The stock open is three statements, building the rows, clearing a field and tail calling
        // the base, so ours needs the field's offset and the base entry point out of it. Both are
        // read rather than assumed, and a shape that does not match aborts instead of guessing.
        auto pOpen = reinterpret_cast<uint8_t*>(JackalFixVTable[1 + nOpenSlot]);
        if (pOpen[nOpenBuildRows] != 0xE8 || pOpen[nOpenBaseTailJump] != 0xE9)
        {
            return false;
        }

        nPageResetField = *reinterpret_cast<int32_t*>(pOpen + nOpenResetFieldDisp);
        BasePageOpen = reinterpret_cast<PageMethod_t>(RelativeTarget(pOpen + nOpenBaseTailJump, 1));

        // The prompt bookkeeping, read out of the base open rather than assumed; see the note
        // beside the offsets. A shape that does not match leaves all three null and the page still
        // works, losing only the lighting of APPLY across a rebuild, which is worth far less than
        // a crash.
        auto pBase = reinterpret_cast<uint8_t*>(BasePageOpen);
        if (pBase[nBaseOpenDirtyOpcode] == 0xC6 && pBase[nBaseOpenDirtyOpcode + 1] == 0x87
            && pBase[nBaseOpenPromptSlot] == 0xE8 && pBase[nBaseOpenSetEnabled] == 0xE8)
        {
            nPageChanged     = *reinterpret_cast<int32_t*>(pBase + nBaseOpenDirtyDisp);
            SetPromptEnabled = reinterpret_cast<SetPromptEnabled_t>(CallTarget(pBase + nBaseOpenSetEnabled));
        }
        else
        {
        }

        // The builder we are replacing is the call this method opens with. We never run it, but its
        // prologue is where the action handler wiring is spelled out.
        auto pStockBuild = CallTarget(pOpen + nOpenBuildRows);
        nHandlerSize     = *reinterpret_cast<uint8_t*>(pStockBuild + nBuildHandlerSize);
        nEventDispatcher = *reinterpret_cast<int32_t*>(pStockBuild + nBuildDispatcherDisp);
        HandlerInit      = reinterpret_cast<HandlerInit_t>(CallTarget(pStockBuild + nBuildHandlerInit));
        DispatcherSlot   = reinterpret_cast<DispatcherSlot_t>(CallTarget(pStockBuild + nBuildDispatcherSlot));
        RegisterHandler  = reinterpret_cast<RegisterHandler_t>(CallTarget(pStockBuild + nBuildRegisterHandler));
        for (size_t i = 0; i < std::size(HandlerVTables); i++)
            HandlerVTables[i] = *reinterpret_cast<uint32_t*>(pStockBuild + nBuildHandlerVTables[i]);

        JackalFixVTable[1 + nOpenSlot] = reinterpret_cast<uintptr_t>(JackalFixOpenPage);
        JackalFixVTable[1 + nClassRecordSlot] = reinterpret_cast<uintptr_t>(JackalFixGetClassRecord);

        // The three slots that are unsafe on a clone; see the note beside their indices above.
        JackalFixVTable[1 + nValueChangedSlot] = reinterpret_cast<uintptr_t>(JackalFixValueChanged);
        JackalFixVTable[1 + nApplySlot]        = reinterpret_cast<uintptr_t>(JackalFixApplyPage);
        JackalFixVTable[1 + nRevertSlot]       = reinterpret_cast<uintptr_t>(JackalFixDefaultPage);

    }

    *reinterpret_cast<uintptr_t**>(pPage) = &JackalFixVTable[1];

    *reinterpret_cast<void**>(pPage + nPageManager) = pManager;
    *reinterpret_cast<void**>(pPage + nPageParent) = pParent;
    SetPageTitle(pPage, szJackalFixPageTitle);

    auto ppSlot = RegistryInsert(pRegistry, nullptr, pJackalFixClassId);
    if (ppSlot == nullptr)
    {
        return false;
    }

    *ppSlot = pPage;
    pJackalFixPage = pPage;
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

    CreateJackalFixPage(static_cast<uint8_t*>(pManager), pParent);

    return pGamePage;
}

static void __fastcall BuildEntries(void* pPage, void* pEdx)
{
    // After the original, so our row lands below Controls.
    BuildEntriesHook.fastcall(pPage, pEdx);

    // The row goes in either way. If the page is missing it is added greyed out rather than
    // skipped, so "no row at all" and "row that does nothing" stay distinguishable symptoms.
    auto pManager = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pPage) + nPageManager);

    void* pDelegate = nullptr;
    if (HasJackalFixPage(pManager))
    {
        if (auto pStorage = GameAlloc(nDelegateSize, 0))
            pDelegate = MakeDelegate(pStorage, nullptr, pPage, pJackalFixClassId, 0, 1);
    }
    else
    {
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

            // CFCXOptionPage::BuildEntries. Entry through the first sub page's delegate allocation;
            // the 7Ch size and the guard test that follows it are unique to this function.
            auto buildPattern = dunia_pattern("83 EC 40 53 55 56 57 33 DB 53 6A 7C 8B F1 E8 ? ? ? ? 8B F8 83 C4 08 3B FB 74 23");
            if (buildPattern.empty())
            {
                return;
            }

            // CListMenuPage::AddValueRow, the two-value row used by every options page. Anchored on
            // the 5Ch control allocation that follows the row append.
            auto rowPattern = dunia_pattern("51 8B 44 24 20 8B 54 24 1C 53 55 56 57 8B 7C 24 18 50 52 57 89 4C 24 1C E8 ? ? ? ? 33 DB 8B E8 53 6A 5C");
            if (rowPattern.empty())
            {
                return;
            }

            // AddSliderRow. Same shape one class along: append the row, then allocate a 50h
            // CSliderSetting rather than a 5Ch two-value control.
            auto sliderPattern = dunia_pattern("8B 44 24 1C 53 56 8B 74 24 0C 57 8B F9 8B 4C 24 24 50 51 56 8B CF E8 ? ? ? ? 8B D8 6A 00 6A 50");
            if (sliderPattern.empty())
            {
                return;
            }

            // AddMultiValueRow. Anchored on the 60h allocation and the vtable store after it.
            auto multiPattern = dunia_pattern("8B 44 24 20 53 55 8B 6C 24 0C 56 57 8B F9 8B 4C 24 2C 50 51 55 8B CF E8 ? ? ? ? 33 DB 53 6A 60");
            if (multiPattern.empty())
            {
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
                return;
            }

            pBuildEntries    = buildPattern.get_first<uint8_t>();
            AddValueRow      = reinterpret_cast<AddValueRow_t>(rowPattern.get_first<uint8_t>());
            AddSliderRow     = reinterpret_cast<AddSliderRow_t>(sliderPattern.get_first<uint8_t>());
            AddMultiValueRow = reinterpret_cast<AddMultiValueRow_t>(multiPattern.get_first<uint8_t>());

            nDelegateSize = *reinterpret_cast<uint8_t*>(pBuildEntries + nBuildDelegateSize);
            MakeDelegate  = reinterpret_cast<MakeDelegate_t>(CallTarget(pBuildEntries + nBuildMakeDelegate));
            AddEntry      = reinterpret_cast<AddEntry_t>(CallTarget(pBuildEntries + nBuildAddEntry));

            // Any instance will do here; the two registry helpers are shared by all of them.
            auto pCreator  = PageCreators.get(0).get<uint8_t>();
            RegistryFind   = reinterpret_cast<RegistryFind_t>(CallTarget(pCreator + nCreateRegistryFind));
            RegistryInsert = reinterpret_cast<RegistryInsert_t>(CallTarget(pCreator + nCreateRegistryInsert));

            // The slot plan is deliberately not built here. It is a function of how many lines the
            // page ends up with, and that is not known until the document exists and the extra rows
            // have been cloned into it, which first happens when the page is opened.
            ResolveLayoutSupport();

            if (auto pRefreshRow = ResolveEngineFunction("ListBox::RefreshRow", 0x00A9AE00,
                "56 57 8B 7C 24 0C 85 FF 8B F1 0F 8C ? ? ? ? 8B 8E 94 00"))
            {
                RefreshRowHook = safetyhook::create_inline(pRefreshRow, JackalFixRefreshRow);
            }

            if (auto pTextDraw = ResolveEngineFunction("Text::DrawString", 0x00AB4350, szTextDrawPattern))
                TextDrawHook = safetyhook::create_inline(pTextDraw, JackalFixTextDraw);

            if (auto pTextDrawPlain = ResolveEngineFunction("Text::DrawStringPlain", 0x00AB4D20, szTextDrawPlainPattern))
                TextDrawPlainHook = safetyhook::create_inline(pTextDrawPlain, JackalFixTextDrawPlain);

            if (auto pImageDraw = ResolveEngineFunction("Image::Draw", 0x00AB93F0, szImageDrawPattern))
                ImageDrawHook = safetyhook::create_inline(pImageDraw, JackalFixImageDraw);

            if (auto pDrawRow = ResolveEngineFunction("ListBox::DrawRow", 0x00A9EC60, szDrawRowPattern))
                DrawRowHook = safetyhook::create_inline(pDrawRow, JackalFixDrawRow);

            if (auto pListDraw = ResolveEngineFunction("ListBox::Draw", 0x00A9F590, szListDrawPattern))
                ListDrawHook = safetyhook::create_inline(pListDraw, JackalFixListDraw);

            auto pReaderRead = ResolveEngineFunction("Reader::Read", 0x00AE7BF0,
                "56 8B F1 83 7E 10 00 57 75 0D 8B 4E 18 8B 01 8B 50 10 FF D2 89 46 10");

            if (pReaderRead != nullptr)
            {
                ReaderReadHook = safetyhook::create_inline(pReaderRead, ReaderRead);
            }
            else
            {
            }

            BuildEntriesHook = safetyhook::create_inline(pBuildEntries, BuildEntries);

            // The ini changing underneath is the other half of keeping the two in step. The plugin
            // already watches the file and re-reads it; this picks the new values up into the rows
            // so an edit made outside the game shows on a page that is already open. Our own
            // writes are skipped, or applying would immediately overwrite the rows with what was
            // just read back.
            JackalFix::onIniFileChange() += []()
            {
                if (bApplyingOurselves || nSlots == 0)
                    return;

                // Our own writes come back round through the file watcher a moment after applying
                // them, by which time the flag saying "this is us" has long been cleared, so the
                // page was rebuilding itself after every APPLY, throwing away where the user was.
                // Rather than race the timing, compare: if the file says what the rows already say,
                // there is nothing to do.
                bool bDiffers = false;
                for (size_t i = 0; i < nSlots && !bDiffers; i++)
                {
                    const auto* pRow = Slots[i].pRow;
                    if (pRow == nullptr || pRow->nKind == ROW_HEADING || pRow->nPref == Pref::COUNT)
                        continue;

                    bDiffers = SettingToRow(*pRow) != PendingValues[i];
                }

                if (!bDiffers)
                    return;

                // Reading the settings into our own array is safe from any thread. Rebuilding the
                // rows is not. This runs on the file watcher's thread, and tearing the rows down
                // while the game is drawing them is a fault inside the row draw. So the values are
                // taken now and the page is redrawn at the next moment we are on the engine's own
                // thread, which is the next key or the next time the page is opened.
                ResetPendingValues();
                bRowsStale = true;

            };

            // Only the creator is resolved here. Its class record is still empty at this point -
            // the creator fills it in the first time it runs, so our own record waits until then.
            if (!ResolveCreator())
                return;

            PageCreatorHook = safetyhook::create_inline(pPageCreator, CreateGamePage);
        };
    }
} JackalFixMenu;
