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
import borderless;    // game window, for measuring what a render resolution percentage is of
import renderconfig;  // which of the game's own quality presets are in force
import debug;         // whether a network session is up, which locks the debug rows

// The options menu is not data driven: options.mgb carries page artwork and navbar prompts, the
// rows are built in code. CFCXOptionPage::BuildEntries appends one row per sub page and hands each
// a delegate holding the target page's class id, resolved by the menu manager's page registry. A
// new page is a sixth append plus a page object registered under our own class id.
//
// Rows are unlimited (magma::ListBox items are a vector; row Y = lineAreaTop + index * (lineHeight
// + spacing)), but values are not: each binds by name to a widget the layout declares and
// MAINMENU_OPTIONGAME_PAGE declares eight. Hence the virtual list, one window handed to the engine
// at a time.

// ------------------------------------------------------------------------------------------------
// Engine object layout.

static constexpr ptrdiff_t nPageList          = 0x0C;  // row list widget, resolved from the document
static constexpr ptrdiff_t nPageDocument      = 0x14;  // page's MAGMA document; widgets looked up in it
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

// magma::ListBox fields this file touches.
//
// nListBoxWrap controls both selection wrap at the ends and whether the edge arrows grey out.
// Cleared deliberately: with it clear, down on the last row leaves the selection alone and the nav
// handler reports the refusal, which is the page-turn signal.
static constexpr ptrdiff_t nListBoxMaxVisible   = 0x18; // byte; visible lines = min(this, item count)
static constexpr ptrdiff_t nListBoxFlags        = 0x19; // bit 0 = wrap at the ends
static constexpr uint8_t   nListBoxWrap         = 0x01;
static constexpr ptrdiff_t nListBoxFirstVisible = 0xCC;
// +44h is the disabled flag; SetSelection refuses such a row, which is what keeps the selection off
// a heading. +38h is the cached visual state computed from the same flag, so a disabled row draws
// greyed. Separate fields, so set the flag and write back an ordinary row's state.
static constexpr ptrdiff_t nListBoxItems       = 0x94;
static constexpr size_t    nItemSize           = 0x54;
static constexpr ptrdiff_t nItemVisualState    = 0x38;
static constexpr ptrdiff_t nItemDisabled       = 0x44;

static constexpr ptrdiff_t nListBoxHighlight    = 0xD0;
static constexpr ptrdiff_t nListBoxSelected     = 0xD4;

// Dispatched from the widget's own table, so this is a per-instance vtable swap, not a hook: every
// list in the game shares the class.
static constexpr size_t nListBoxNavSlot    = 27; // +6Ch
static constexpr size_t nListBoxHoverSlot  = 28; // +70h, pointer moving over a row
static constexpr size_t nListBoxMouseSlot  = 29; // +74h, click
static constexpr size_t nListBoxPointerSlot = 30; // +78h, the other pointer entry

// The function that lights a row is called from slots 27-30 and nowhere else, so those four are
// every way a row can be lit.
static constexpr size_t nListBoxVTableSlots = 45;

// What the nav handler writes into its result when it could not move the selection: a direction
// code and a flag asking the focus manager to pass the input on. Swallowing that flag stops a page
// turn from also moving focus off the list.
static constexpr ptrdiff_t nNavResultCode   = 0x10;
static constexpr ptrdiff_t nNavResultFlags  = 0x16;
static constexpr uint8_t   nNavFlagUnhandled = 0x04;
static constexpr int       nNavCodeUp        = 0;
static constexpr int       nNavCodeDown      = 1;

// Page vtable slots. 1 and 2 are the same on every page class here; 16 is the row-clearing entry
// the stock builders call before repopulating.
//
// 19, 20 and 21 must be owned by a clone. 20 and 21 are the option layer's Apply and Revert,
// reached from the generic action handlers at 0x1087EB10, 0x1087F090 and 0x1087F1F0. 19 is
// value-changed, raised on every arrow press; CFCXOptionGamePage uses it to show or hide the
// machete row with difficulty.
//
// All three read row indices stashed at page+1D8h..1F8h, still -1 on a clone. The lookup is a
// std::map::operator[], so it inserts a null against key -1 and returns a pointer to it:
//
//     control = *map[page->difficultyRow];
//     if (control == 0) control = 0;          // "handled"
//     (**(code**)(*control + 0x38))();        // 1081F70Ch, dereferenced regardless
//
// Owning them is what makes the stock action handlers safe to reuse.
static constexpr size_t nClassRecordSlot  = 1;
static constexpr size_t nOpenSlot         = 2;
static constexpr size_t nClearRowsSlot    = 16;
static constexpr size_t nValueChangedSlot = 19; // +4Ch
static constexpr size_t nApplySlot        = 20; // +50h
static constexpr size_t nRevertSlot       = 21; // +54h

// Copied wholesale so the clone keeps every behaviour not deliberately changed. The real table is
// 26 entries; the extra slots are never dispatched.
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

// The stock row builder opens by registering three handlers against the page's action dispatcher:
// Apply, Revert and Back. Replacing the builder wholesale left our page with none, and the input
// path then dispatches into a null object on the first prompt press. Everything needed to register
// them is read out of the builder's prologue.
static constexpr ptrdiff_t nBuildHandlerSize     = 0x0E; // push 0Ch, the handler allocation size
static constexpr ptrdiff_t nBuildHandlerInit     = 0x22; // call <handler base ctor>
static constexpr ptrdiff_t nBuildDispatcherDisp  = 0x37; // disp32 of lea ebp,[esi+disp32]
static constexpr ptrdiff_t nBuildDispatcherSlot  = 0x3F; // call <slot for event id>
static constexpr ptrdiff_t nBuildRegisterHandler = 0x46; // call <register>
static constexpr ptrdiff_t nBuildHandlerVTables[]{ 0x29, 0x66, 0x9D }; // mov [edi], <vtable>

// The base open's prologue is the only place all three prompt fields appear together:
//
//     mov  byte ptr [edi+1B8h], 0     ; nothing changed yet
//     push 0 / push 1 / call <slot>   ; prompt one is APPLY
//     call <set enabled>              ; starts greyed
//     push 1 / push 2 / call <slot>   ; prompt two is DEFAULT
//     call <set enabled>              ; starts lit
//
// The byte at 1B8h is the mechanism: the per-row change handler lights APPLY only while it is
// clear, the APPLY handler refuses to run while it is clear, and the confirmation box behind
// DEFAULT clears it again. Our rebuild runs through the same base open, so all three are read out
// rather than assumed.
static constexpr ptrdiff_t nBaseOpenDirtyOpcode = 0x1B; // mov byte ptr [edi+disp32], 0
static constexpr ptrdiff_t nBaseOpenDirtyDisp   = 0x1D;
static constexpr ptrdiff_t nBaseOpenPromptSlot  = 0x22; // call <slot for event id>
static constexpr ptrdiff_t nBaseOpenSetEnabled  = 0x29; // call <light or grey that prompt>

// Which prompt is which, in the numbering the dispatcher uses.
static constexpr int nPromptApply   = 1;
static constexpr int nPromptDefault = 2;

// ------------------------------------------------------------------------------------------------
// Engine functions. The __thiscall ones are declared __fastcall with an unused EDX: same
// convention, hidden argument spelled out.

using GameAlloc_t      = void*    (__cdecl*)(size_t nSize, int nUnused);
using PageConstruct_t  = void*    (__fastcall*)(void* pPage);
using PageMethod_t     = void     (__fastcall*)(void* pPage, void* pEdx);
using RegistryFind_t   = void     (__fastcall*)(void* pRegistry, void* pEdx, void** ppIterator, const uint32_t* pClassId);
using RegistryInsert_t = void**   (__fastcall*)(void* pRegistry, void* pEdx, const uint32_t* pClassId);
using MakeDelegate_t   = void*    (__fastcall*)(void* pDelegate, void* pEdx, void* pOwnerPage, const uint32_t* pClassId, int nUnused, char bSetParent);
using AddEntry_t       = int      (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pText, int bEnabled, void* pDelegate);

// Appends a row carrying a two-value control (the < Yes / No > arrows): AddEntry plus a control
// stamped from pTemplate into the page's document, registered in a map at page+190h. That map is
// why this only works on pages derived from the value-page base: a plain CListMenuPage is 190h
// bytes and has no such member.
using AddValueRow_t    = void*    (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pLabel, const char* pTemplate,
                                                const char* pWidgetName, const wchar_t* pOnText, const wchar_t* pOffText,
                                                int bEnabled, void* pDelegate);

// A list of arbitrary choices; count comes before both arrays. Both are copied into the control as
// it is built (labels into the widget's item list, values into an int array at control+4Ch), so
// neither has to outlive the call. Still draws as arrows, not a popup.
using AddMultiValueRow_t = void*  (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pLabel, const char* pTemplate,
                                                const char* pWidgetName, uint32_t nCount,
                                                const wchar_t* const* pLabels, const int* pValues,
                                                int bEnabled, void* pDelegate);

// Selects the value whose byte matches, by searching the control's own value list.
using SetRowValue_t    = void     (__fastcall*)(void* pControl, void* pEdx, const void* pValue);
using GetRowValue_t    = const void* (__fastcall*)(void* pControl, void* pEdx);
static constexpr size_t nRowSetValueSlot = 13;
static constexpr size_t nRowGetValueSlot = 14;

// magma::ListBox's input handler, taken over per instance. pResult is where it records that it
// could not act on the key.
using ListBoxNav_t = int (__fastcall*)(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult);

static GameAlloc_t        GameAlloc        = nullptr;
static PageConstruct_t    PageConstruct    = nullptr;
static RegistryFind_t     RegistryFind     = nullptr;
static RegistryInsert_t   RegistryInsert   = nullptr;
static MakeDelegate_t     MakeDelegate     = nullptr;
static AddEntry_t         AddEntry         = nullptr;
static AddValueRow_t      AddValueRow      = nullptr;
static AddMultiValueRow_t AddMultiValueRow = nullptr;
static PageMethod_t       BasePageOpen     = nullptr;

// The stock call site pushes handler then event id and calls both in turn, each taking one stack
// argument: the slot call consumes the event id and leaves the handler for the register call.
// Reading it as one two-argument call unbalances the stack by eight bytes.
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

// Every menu page comes from the same template function, so any instance resolves the shared
// registry helpers. The one building the page we clone is picked out by the class id it pushes.
static hook::pattern PageCreators{};
static uint8_t* pBuildEntries = nullptr;
static uint8_t* pPageCreator = nullptr;

// ------------------------------------------------------------------------------------------------
// What we clone, and where its widgets come from.
//
// The clone is a CFCXOptionGamePage, not the plain CFCXOptionPage the OPTIONS list uses: the row
// APIs need the value-page base to register their controls in.
//
// CListMenuPage resolves its document from a hash of its MAGMA page name, so two page objects
// naming the same MAGMA page share one document, list widget and selected index. Hence the clone
// names a page of its own. MAINMENU_OPTIONGAME_PAGE is the console cut of game options: it ships in
// the PC options.mgb (whose .desc carries a navbar block for it), but the PC build only ever names
// MAINMENU_OPTIONGAME_PAGE_PC and the plain name appears nowhere in Dunia's strings.
static const char szStockPageLayout[]     = "MAINMENU_OPTIONGAME_PAGE_PC";
static const char szJackalFixPageLayout[] = "MAINMENU_OPTIONGAME_PAGE";

static const wchar_t szJackalFixPageTitle[] = L"JACKAL FIX";

// Widget template each value row is stamped from.
static const char szRowTemplate[] = "SETTING_LABEL_LIST";

// A value row binds by name to a widget the layout declares. The bind call looks the name up,
// stores the result on the control, and on failure silently skips adding the choices, drawing
// arrows with nothing between them.
static constexpr ptrdiff_t nControlNamedWidget = 0x44; // the widget a value row found by name

// How far into the constructor to look for the layout name. Generous and bounded.
static constexpr ptrdiff_t nCtorScanBytes = 0x600;

// ------------------------------------------------------------------------------------------------
// The layout's lines.
//
// Labels are dynamic rows in the page's nav list; value widgets are fixed layout furniture, each
// drawn on the line the layout assigns it, so row N lines up only with the widget on line N.
//
// MAINMENU_OPTIONGAME_PAGE's settings map holds ten entries binding a SETTING_* name to a path
// ending in a row container p_setting_1..8, at Y 225, 250, 278, 306, 335, 364, 391 and 420. Sorted
// by Y they give the order below; each container's child link says whether the line holds arrows
// (p_list) or a slider.
//
// One slider line per eight, and no row here pairs with a slider, so numeric settings are declared
// as ranges (ROW_RANGE) and drawn with arrows. The package patch repoints the slider line at the
// arrows template; without the patch the line is marked unusable and left blank.
struct PageLine
{
    const char* pWidgetName;
    bool        bUsable;
};

// The eight the layout ships with. SETTING_9 and SETTING_10 are unusable: SETTING_9's link has a
// component count of zero and SETTING_10 is absent from the map, so either fails the name lookup,
// leaving a null widget pointer and a blank row.
static const PageLine StockLines[]
{
    { "SETTING_1",           true  }, // p_setting_1, y 225
    { "SETTING_SENSITIVITY", false }, // p_setting_2, y 250, the lone slider line
    { "SETTING_3",           true  }, // p_setting_3, y 278
    { "SETTING_4",           true  }, // p_setting_4, y 306
    { "SETTING_5",           true  }, // p_setting_5, y 335
    { "SETTING_6",           true  }, // p_setting_6, y 364
    { "SETTING_7",           true  }, // p_setting_7, y 391
    { "SETTING_8",           true  }, // p_setting_8, y 420
};

// ------------------------------------------------------------------------------------------------
// How far the title and row block move up, in MAGMA layout units (the package resolves at 1280x800,
// so a unit is an 800th of screen height at any resolution). Measured by eye to put the title where
// ACTION sits on the controls page.
static constexpr int    nLiftUnits   = 94;

// The stock eight plus the patched-in rows, with headroom.
static constexpr size_t nMaxLines = 24;

static PageLine RowLines[nMaxLines]{};
static size_t   nRowLines = 0;

// The widget each line draws its value in, and the y it belongs at. A line whose row carries no
// value has its arrows parked off screen, put back when a value row next lands there.
static void*   LineWidgets[nMaxLines]{};
static int16_t LineY[nMaxLines]{};

// The widget name each line binds to. Held here because patched-in row names are built at run time.
static char LineNames[nMaxLines][24]{};

// Which line a widget is the value of, or -1 for anything that is not one of this page's values.
static int ValueWidgetLine(const void* pWidget)
{
    for (size_t i = 0; i < nRowLines; i++)
    {
        if (pWidget != nullptr && LineWidgets[i] == pWidget)
            return static_cast<int>(i);
    }
    return -1;
}

// Off the bottom of the page. Used instead of a visibility flag: nothing about a parked widget
// changes except its position.
static constexpr int16_t nParkedY = 4000;

// A row's value widget is the innermost list in options / p_option_game / p_setting_N / p_list /
// l_setting, whose own coordinate is zero: the per-row offset lives on the container above, which
// the name does not resolve to. So every row reads as y 0.

// ------------------------------------------------------------------------------------------------
// The page's contents, declared per ini section and in ini order.

enum RowKind
{
    ROW_HEADING, // label with no control; consumes a line, whose widget draws faded
    ROW_BOOL,    // two-value arrows reading On / Off
    ROW_ENUM,    // a named list of choices
    ROW_RANGE,   // a numeric range, drawn as a generated list of steps
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

    // ROW_RANGE. The control is integral, so a float setting is carried in fixed point: the row
    // works in units of the ini value times fScale and converts at the edges. Min, max and step are
    // in those units.
    int   nMinimum;
    int   nMaximum;
    int   nStep;
    float fScale;
    const wchar_t* pFormat; // how a step is written out, e.g. L"%d" or L"%.2f"

    // ROW_ENUM. Both arrays are copied by the engine as the row is built, so static storage suffices.
    const int*            pValues;
    const wchar_t* const* pValueLabels;
    uint32_t              nValueCount;

    // A second setting moved in step with the first, where one row stands for a pair (an internal
    // resolution is one choice and two ini keys). Its value is the matching entry of pValues2.
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

// Boolean over a setting the ini carries as a float: the two states write 0 and 1 as floats.
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

static constexpr MenuRow Range(const wchar_t* pLabel, Pref nPref, const char* pSection, const char* pKey,
                               ValueKind nValue, int nMinimum, int nMaximum, int nStep,
                               float fScale = 1.0f, const wchar_t* pFormat = L"%d")
{
    return { ROW_RANGE, pLabel, nPref, pSection, pKey, nValue,
             nMinimum, nMaximum, nStep, fScale, pFormat, nullptr, nullptr, 0, Pref::COUNT, nullptr, nullptr };
}

// ------------------------------------------------------------------------------------------------
// Choice lists.

static const int     DisplayModeValues[]{ 1, 2, 3 };
static const wchar_t* const DisplayModeLabels[]{ L"Fullscreen", L"Borderless", L"Windowed" };

static const int     ScalingFilterValues[]{ 0, 1 };
static const wchar_t* const ScalingFilterLabels[]{ L"Point", L"Bilinear" };

// Render Resolution is InternalResolutionX/Y. The row lists percentages of that pair, both axes
// scaled together to keep the aspect, each choice writing both keys. No "off": 100% is the pair
// itself, and an unset pair already means the window.
static constexpr int nRenderScaleStep = 10;
static constexpr int nRenderScaleFloor = 20;
static constexpr int nRenderScaleCeiling = 200;

// The Xbox 360 cut, offered when the base is 16:9. Not a percentage, so it heads the list as its
// own pair.
static constexpr int nConsoleWidth = 1280;
static constexpr int nConsoleHeight = 696;

// Smallest frame the engine copes with, and the largest square a D3D9 render target can describe.
static constexpr int nRenderPixelMinW = 320;
static constexpr int nRenderPixelMinH = 240;
static constexpr int nRenderPixelMax = 16384;

// Filled in as the page opens, since the base pair is only known then. One entry per step.
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

// Geometry has a 6x step the others lack, so it cannot share the list above.
static const int     BeyondUltraGeometryValues[]{ 0, 1, 2, 3, 4 };
static const wchar_t* const BeyondUltraGeometryLabels[]{ L"Default", L"2x", L"4x", L"6x", L"Max draw distance" };

static const int     BeyondUltraShadowValues[]{ 0, 1 };
static const wchar_t* const BeyondUltraShadowLabels[]{ L"Default", L"Max draw distance" };

static const int     SkipTutorialsValues[]{ 0, 1, 2 };
static const wchar_t* const SkipTutorialsLabels[]{ L"Off", L"Pop-ups", L"Pop-ups and hints" };

// Zero means every CPU, so it gets a word rather than a number.
static const int     CpuAffinityValues[]{ 0, 1, 2, 4, 6, 8, 12, 16 };
static const wchar_t* const CpuAffinityLabels[]{ L"All CPUs", L"1", L"2", L"4", L"6", L"8", L"12", L"16" };

// ------------------------------------------------------------------------------------------------
// The sections, in the order the ini declares them.

// Not const: the render resolution row's ends are settled against the window at page open.
static MenuRow DisplayRows[]
{
    Heading(L"DISPLAY"),
    Enumeration(L"Display Mode", PREF_DISPLAYMODE, "Display", "DisplayMode", DisplayModeValues, DisplayModeLabels),
    // Built by hand rather than through Enumeration: the choices are filled in at page open, so the
    // count is settled there (a helper's would be the array capacity). Also the one row carrying a
    // second ini key, chosen by the same index.
    { ROW_ENUM, L"Render Resolution", PREF_INTERNALRESOLUTIONX, "Display", "InternalResolutionX",
      VALUE_INT, 0, 0, 1, 1.0f, nullptr, RenderChoiceX, RenderChoiceLabels, 0,
      PREF_INTERNALRESOLUTIONY, "InternalResolutionY", RenderChoiceY },
    Enumeration(L"Scaling Filter", PREF_SCALINGFILTER, "Display", "ScalingFilter", ScalingFilterValues, ScalingFilterLabels),
    Enumeration(L"Max Frame Rate", PREF_MAXFRAMERATE, "Display", "MaxFrameRate", MaxFrameRateValues, MaxFrameRateLabels),
    Boolean(L"FPS Counter", PREF_FPSCOUNTER, "Display", "FpsCounter"),
};

static const MenuRow GraphicsRows[]
{
    Heading(L"GRAPHICS"),
    Boolean(L"Improved Utilisation", PREF_UTILISATION, "Graphics", "ImprovedUtilisation"),
    Enumeration(L"Anisotropic Filtering", PREF_ANISOTROPICFILTERING, "Graphics", "AnisotropicFiltering",
                AnisotropyValues, AnisotropyLabels),
    Boolean(L"No Rim Lighting", PREF_NORIMLIGHTING, "Graphics", "NoRimLighting"),
    Range  (L"Saturation", PREF_SATURATION, "Graphics", "Saturation", VALUE_FLOAT, 0, 100, 5, 100.0f, L"%.2f"),
};

static const MenuRow BeyondUltraRows[]
{
    Heading(L"BEYOND ULTRA"),
    Enumeration(L"Geometry", PREF_BEYONDULTRAGEOMETRY, "BeyondUltra", "BeyondUltraGeometry", BeyondUltraGeometryValues, BeyondUltraGeometryLabels),
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
    Boolean(L"No Hit Indicator",  PREF_NOHITINDICATOR,   "Gameplay", "NoHitIndicator"),
};

// Hundredths and "%g", not whole degrees and the default "%d": "%d" handed a double prints 0, since
// the low 32 bits of a whole number of degrees as a double are zero. Hundredths because the ini may
// hold 91.35; "%g" writes 91.35 as "91.35" and 95 as "95".
//
// Steps of five: 95 steps between 45 and 140 is not drivable with an arrow key. Both clamps are
// multiples of five, and the step builder keeps the ini's own value wherever it falls, so 91.35
// sits between 90 and 95 and one press lands on the ladder.
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

// The base is InternalResolutionX/Y read from the file, not from what is in force, so a row already
// moved this session does not become the base for the next percentage. An unset pair means the
// window, measured rather than assumed: borderless makes the window and the video options disagree.
//
// The list walks outwards from 100 and stops at the first step outside the pixel limits (20% of
// 640x480 is a frame the engine refuses; 200% of 15360x8640 exceeds D3D9). 100 is always offered.
static void SettleRenderResolutionChoices()
{
    auto nBaseW = JackalFixSettings.GetFileInt(PREF_INTERNALRESOLUTIONX);
    auto nBaseH = JackalFixSettings.GetFileInt(PREF_INTERNALRESOLUTIONY);

    // 100% of the window is written back as unset, not as a pixel count, so the pair keeps meaning
    // "follow the window".
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

    // Even pixels: the engine's half resolution passes trip over an odd render target.
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

    // Exactly 16:9.
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
// What DEFAULT restores: the values in the ini as distributed, not whatever the file holds now.
// In the units the row works in, so a fixed-point float is written scaled (saturation 0.5 at scale
// 100 is 50).

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
    { PREF_FPSCOUNTER,              0   },

    { PREF_ANISOTROPICFILTERING,    16  },
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
    { PREF_NOHITINDICATOR,          0   },

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
// Every declared row gets a slot; slot S is drawn on line S modulo the line count. Built once: a
// row takes the next slot whose line can carry it, blanks fill anything skipped, and a heading is
// pushed to the next page rather than stranded on the last line of one. Scrolling then picks which
// run of slots to hand the engine. Bounded by the declarations above, not by anything at runtime.
static constexpr size_t nMaxSlots = 256;

// One entry per slot, null for a blank.
static const MenuRow* Slots[nMaxSlots]{};
static size_t   nSlots = 0;
static size_t   nPages = 0;

// One entry per slot, holding the value chosen but not yet applied. A page turn destroys the
// controls, so a value living only in a control would be lost when the next page is drawn.
static int  PendingValues[nMaxSlots]{};
static bool bPendingValid = false;

static bool IsUsableLine(size_t nSlot)
{
    return RowLines[nSlot % nRowLines].bUsable;
}

// Without a usable line the search below fills the plan with blanks and nPages divides by zero.
static bool AnyUsableLine()
{
    for (size_t i = 0; i < nRowLines; i++)
    {
        if (RowLines[i].bUsable)
            return true;
    }
    return false;
}

static void PlanSlots()
{
    if (nSlots != 0 || !AnyUsableLine())
        return;

    for (const auto& section : MenuSections)
    {
        if (section.nRows == 0)
            continue;

        // A heading is worth a line only if its first row can follow it on the same page. Only the
        // first: a section longer than a page runs over anyway, and requiring all of it to fit
        // would push the heading down forever.
        size_t nHeading = nSlots;
        if (section.nRows > 1)
        {
            size_t nTarget = nHeading + 1;
            while (nTarget < nMaxSlots && !IsUsableLine(nTarget))
                nTarget++;

            if (nTarget / nRowLines != nHeading / nRowLines)
                nHeading = (nHeading / nRowLines + 1) * nRowLines;
        }

        while (nSlots < nHeading && nSlots < nMaxSlots)
            Slots[nSlots++] = nullptr;

        if (nSlots >= nMaxSlots)
            break;

        Slots[nSlots++] = &section.pRows[0];

        for (size_t i = 1; i < section.nRows; i++)
        {
            while (nSlots < nMaxSlots && !IsUsableLine(nSlots))
                Slots[nSlots++] = nullptr;

            if (nSlots >= nMaxSlots)
                break;

            Slots[nSlots++] = &section.pRows[i];
        }
    }

    // Round out the last page so the window never runs off the end of the plan.
    while (nSlots % nRowLines != 0 && nSlots < nMaxSlots)
        Slots[nSlots++] = nullptr;

    nPages = nSlots / nRowLines;
}

// ------------------------------------------------------------------------------------------------
// What was actually built, so Apply and the page turn can walk it without asking the engine's row
// map. That map is a std::map whose operator[] inserts a null on a miss and hands it back.

struct BuiltRow
{
    size_t nSlot;
    void*  pControl;
};

// Defined with the navigation, the only other thing that moves the selection.
static void SettleSelection(void* pListBox, int nFrom, int nDirection);
static void RefreshIfStale(void* pPage);
static void ShowPage(void* pPage, size_t nPage, bool bKeepOnScreenValues, int nTurnDirection);

static BuiltRow BuiltRows[nMaxLines]{};
static size_t   nBuiltRows = 0;
static size_t   nCurrentPage = 0;

// ------------------------------------------------------------------------------------------------
// Engine memory whose shape is inferred rather than known is read through here, so a wrong offset
// costs a zero instead of a crash.
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

// An immediate is only dereferenced once it is known to land inside Dunia's image.
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
// options.mgb declares eight value widgets and the page can show no more. Nothing at runtime adds a
// ninth: the duplication machinery only instantiates the Area family, a settings row is a
// magma::Element, so the object server returns null and BeginClone dereferences it at 0x10AEC57C.
//
// So the rows are added in the layout, by patching the bytes on their way into the parser rather
// than by shipping a modified options.mgb. The file is read into one contiguous heap buffer before
// any of it is parsed, so substituting that buffer substitutes the file and nothing reaches disk.
//
// The format allows it: a sequential object graph with no absolute offsets, no size fields and no
// checksum, so bytes can be inserted anywhere provided every enclosing count is corrected. Four
// edits do it: the new row containers, their entries in the page's name map, and the two counts
// above them.
//
// Every offset is found by walking the file, since the localised copies are different sizes. If any
// anchor fails to resolve the buffer is left as it was.

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

// The eight the file ships with run y 225 to 420 at a spacing of 28. These carry on from them.
static constexpr uint16_t nExtraRowFirstY = 448;
static constexpr uint16_t nExtraRowStep   = 28;

// Nine puts the last row clear of the Back / Default / Apply prompts once the lift is applied,
// which is as many as the page has room for.
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

// Steps over one { key, tag, payload }, returning where the next begins. Zero for a tag this build
// does not know: the payload length depends on the tag, so the walk has lost the stream and
// guessing at it would corrupt the file.
static uint32_t StepMapEntry(const uint8_t* pFile, uint32_t nLength, uint32_t nAt)
{
    if (nAt + 8 > nLength)
        return 0;

    auto nTag = ReadU32(pFile, nAt + 4);
    nAt += 8;

    switch (nTag)
    {
    case 0x02: case 0x07: nAt += 4; break;   // int, float
    case 0x0C:            nAt += 1; break;   // bool
    case 0x13:            nAt += 8; break;   // a pair of words
    case 0x14:                       break;  // nothing at all
    case 0x10:                               // length prefixed ascii
        if (nAt + 4 > nLength)
            return 0;
        nAt += 4 + ReadU32(pFile, nAt);
        break;
    case 0x11: case 0x12: case 0x15:         // a path
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

// Steps over a name map: a count, then that many entries.
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
        nAt = StepMapEntry(pFile, nLength, nAt);
        if (nAt == 0)
            return false;
    }

    nEnd = nAt;
    return true;
}

// Where the patch has to write, found by walking. The localised copies of this file are different
// sizes, so none of these offsets could be written down.
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

    // The package name, at a position derived from the type table rather than assumed.
    auto nBody = 0x0F + 4u * (static_cast<uint32_t>(pFile[0x0E]) - 1);
    auto nPackageName = nBody + 65u * 4u;
    if (nPackageName + 4 > nLength)
        return false;

    return ReadU32(pFile, nPackageName) == Crc32Of("options");
}

static bool FindPackageSites(const uint8_t* pFile, uint32_t nLength, PackageSites& Sites)
{
    Sites = {};

    // The page whose map names the settings rows. The same name appears more than once in the file,
    // so the shape of what follows settles it.
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

    // The rows themselves. Plenty of other records begin with the same type byte, so the search is
    // anchored on the names this page's own map points at.
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

    // Other pages in the file reuse these row names, so neither a name nor a coordinate finds this
    // page's block alone. What does is a contiguous run of records sharing one x that covers most
    // of the map's rows: a stray single record fails it, and the other page's block sits later.
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

// One row container, in the shape the file's own records have. Every byte not named here is zero in
// the originals. The constants in the middle are copied verbatim, their meaning not established,
// which is why the caller checks them against a real record first.
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

// Names of the rows the patch adds, so the layout side can ask for the same ones.
static void ExtraRowName(char* pOut, size_t nSize, int nIndex) { snprintf(pOut, nSize, "p_setting_%d", 11 + nIndex); }
static void ExtraRowKey (char* pOut, size_t nSize, int nIndex) { snprintf(pOut, nSize, "SETTING_%d",   11 + nIndex); }

// Builds the replacement image. Zero if anything did not add up, and the caller then leaves the
// original bytes alone and the page runs on its eight rows.
static uint32_t BuildPatchedPackage(const uint8_t* pFile, uint32_t nLength, uint8_t* pOut, uint32_t nOutCapacity)
{
    PackageSites Sites{};
    if (!FindPackageSites(pFile, nLength, Sites))
        return 0;

    // The record shape is copied from a real row, so prove one exists in that form before emitting
    // nine more like it.
    auto nSample = Sites.nRowsEnd - nRowRecordSize;
    if (ReadU32(pFile, nSample + 67) != 0xEC705196 || pFile[nSample + 71] != 0xE5 || pFile[nSample + 90] != 0xFF)
        return 0;

    auto nRows = static_cast<uint32_t>(nExtraRows);
    auto nAddedRows = nRows * static_cast<uint32_t>(nRowRecordSize);
    auto nAddedMap  = nRows * static_cast<uint32_t>(nMapEntrySize);
    auto nNewLength = nLength + nAddedRows + nAddedMap;
    if (nNewLength > nOutCapacity)
        return 0;

    // Three runs, so the two insertions land without disturbing anything else.
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

        // The id is stable for a name within a page but differs for the same name across pages, so
        // it is not a hash of the name alone. Unique is all that can be said for what goes here.
        auto nId = Crc32Of(szRow) ^ 0x5A5A5A5Au;

        uint8_t Record[nRowRecordSize]{};
        MakeRowRecord(Record, szRow, Sites.nRowX,
            static_cast<uint16_t>(nExtraRowFirstY + nExtraRowStep * i - nLiftUnits), nId);
        Append(Record, nRowRecordSize);
    }

    Append(pFile + Sites.nRowsEnd, nLength - Sites.nRowsEnd);

    // The lift happens here rather than by moving each value widget at run time. A widget's own
    // coordinate is an offset inside its container, and the container is what the mouse hit-tests
    // against, so moving the widget left the arrows workable only with the keyboard.
    for (auto nRow = Sites.nRowsBegin + nAddedMap; nRow < Sites.nRowsEnd + nAddedMap; nRow += nRowRecordSize)
    {
        auto nY = ReadU16(pOut, nRow + nRowY);
        WriteU16(pOut, nRow + nRowY, static_cast<uint16_t>(nY - nLiftUnits));
    }

    // The header carries sixty-five pool sizes and the parser allocates every object out of them.
    // They are sized to exactly what the stock file needs, and the allocator pops a free list head
    // without checking it, so extra elements against unchanged counts dereference null part way
    // through loading the package. Which pool belongs to which class is not established, so every
    // non-empty one gets headroom; an unused entry costs one object's worth of memory.
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

    // The two counts above what was inserted. The child count sits after the map, so it has moved
    // by the bytes the map grew by.
    WriteU32(pOut, Sites.nMapCount, ReadU32(pOut, Sites.nMapCount) + nRows);
    WriteU32(pOut, Sites.nChildCount + nAddedMap, ReadU32(pOut, Sites.nChildCount + nAddedMap) + nRows);

    // Nothing on this page pairs with a slider, so the slider line is a blank in the middle of
    // every screen. Pointed at the arrows template it becomes another usable row. Its map entry is
    // repointed to match, or the path still leads to the slider.
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
// The hook sits on the buffered reader itself. Hooking the package loader and walking from the
// archive down to the reader was tried first: one offset in that chain was wrong, the reader
// pointer came out null and every test after it fell through in silence. Here the reader is the
// object the hook is called on, so there is no chain to get wrong.
//
// It runs after the original, by which point the reader has pulled the whole file into one heap
// block and copied the caller's bytes out of it. Swapping the block then is safe: the patch only
// inserts well past the header, so everything read so far is identical in both copies. The read
// position is left alone so the parse carries on where it was.

static constexpr ptrdiff_t nReaderBuffer   = 0x04;
static constexpr ptrdiff_t nReaderBuffered = 0x08;
static constexpr ptrdiff_t nReaderPosition = 0x0C;
static constexpr ptrdiff_t nReaderSize     = 0x10;

// Only a file that has barely been read is worth looking at, which is every file exactly once.
static constexpr uint32_t nReaderEarly = 32;

// Not the allocator the pages are built with. Pairing that one with the free this block eventually
// meets would corrupt the heap on the way out.
// The GOG build moved it, along with everything else in that region.
static uint32_t EngineAllocSlot() { return ByBuild<uint32_t>(0x00FB6440, 0x00F0F3F0); }

using EngineAlloc_t = void* (__cdecl*)(size_t nSize, int nFlags);

static SafetyHookInline ReaderReadHook{};

// Read at the moment of use, never cached. The slot holds a placeholder until the engine installs
// its real allocator, long after a plugin initialises, so a cached copy hands back null for ever.
static EngineAlloc_t EngineAllocator()
{
    if (hDunia == nullptr)
        return nullptr;

    auto ppAllocator = reinterpret_cast<EngineAlloc_t*>(reinterpret_cast<uint8_t*>(hDunia) + EngineAllocSlot());
    if (!IsReadable(ppAllocator, sizeof(void*)))
        return nullptr;

    auto pAllocator = *ppAllocator;
    return PointsIntoDunia(reinterpret_cast<void*>(pAllocator)) ? pAllocator : nullptr;
}

// The added rows carry names the stock file does not, so the first of them marks a patched buffer.
// Cheaper than tracking the blocks handed out, and correct if the engine reloads into an old one.
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
        return;

    auto nPatched = BuildPatchedPackage(pBuffer, nLength, pPatched, nCapacity);
    if (nPatched == 0)
        return;

    // Handed over rather than copied over. The archive owns this block now and frees it with the
    // allocator it came from. The original is leaked deliberately: freeing it means calling a
    // second engine function on trust, for one fifty kilobyte copy per menu build.
    *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pReader) + nReaderBuffer) = pPatched;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pReader) + nReaderSize) = nPatched;
}

// ------------------------------------------------------------------------------------------------
// Reshaping the page.
//
// The stock layout starts too far down, and moving it is all that happens here. A widget's position
// lives on the magma::State at widget+08h and is written only by the keyframe evaluator, so a poke
// holds until the next evaluation and is then undone. The lock mask at widget+0Ch is the engine's
// own answer: Widget::OnPropertyChanged sets these bits when a script assigns POSITION.Y and every
// state evaluator tests them before writing its channel, so setting them makes the move permanent
// rather than a race with the animation.
//
// More rows does not happen here. The duplication machinery only instantiates the Area family
// (Area, Page, Button, CheckBox, Cursor), and both l_setting and the p_setting_N container above it
// are magma::Elements, so the object server returns null and BeginClone dereferences it. The extra
// rows come from the package patch above.
//
// Every engine function here is checked against its own first bytes before it is called. A mismatch
// leaves the page drawing where the layout put it rather than crashing.

// magma::Widget.
static constexpr ptrdiff_t nWidgetState    = 0x08;
static constexpr ptrdiff_t nWidgetLockMask = 0x0C;
static constexpr uint32_t  nLockPositionY  = 0x00000C04; // pos.y, rect.top, rect.bottom

// magma::State. A ListBox and an AreaInstance carry a ScaleState, +24h x and +26h y. A Text carries
// a TextState, which is a RectState, and there +24h..+2Ah are left, right, top and bottom. The
// difference is silent, so each kind of widget is written in its own terms.
static constexpr ptrdiff_t nStateScaleY     = 0x26;
static constexpr ptrdiff_t nStateRectTop    = 0x28;
static constexpr ptrdiff_t nStateRectBottom = 0x2A;

// magma::Element, and the property container every named object hangs its name table from.
static constexpr ptrdiff_t nOwnerProperties   = 0x0C;
static constexpr ptrdiff_t nElementWidget     = 0x14;

// The page's title text widget, resolved by the base widget pass alongside the row list.
static constexpr ptrdiff_t nPageTitleWidget   = 0x10;

// magma::FullLink, the payload a name-table entry of tag 12h carries. Its dword array alternates
// resolved node pointers with path hashes; a name resolves to the last node, index 2m for m =
// (count - 1) / 2.
static constexpr ptrdiff_t nFullLinkBegin    = 0x08;
static constexpr ptrdiff_t nFullLinkEnd      = 0x0C;
static constexpr ptrdiff_t nVariantTag       = 0x04;
static constexpr ptrdiff_t nVariantPayload   = 0x08;
static constexpr int32_t   nVariantLinkTag   = 0x12;

// The panel the heading, its rule and the row list sit inside, and the bar the heading and its rule
// share. Neither is in the document's name table, which holds twenty-one entries and every one a
// magma::ListBox, so both are reached by walking a parent's children. See FindArea below.
static const char szNavPanel[] = "p_menu_nav";
static const char szTitleBar[] = "a_title_bar";

using Crc32_t             = uint32_t (__cdecl*)(uint32_t* pOut, const char* pText);
using NameLookup_t        = void*    (__fastcall*)(void* pContainer, void* pEdx, uint32_t nCrc);

// Moves the selection, refreshing the ink on the row it left and the row it landed on. Needed
// because a heading is an ordinary selectable row to the engine.
using SetSelection_t      = int      (__fastcall*)(void* pListBox, void* pEdx, int nIndex, int bRefresh, int bScroll);

// The highlight is not the selection. +D4h is the selected row, +D0h the lit one, and they are set
// by different calls, so moving the selection alone changes nothing on screen. This is the call
// that lights a row, and what a mouse click uses to light a value widget. Index -1 releases it.
using SetHighlight_t      = void     (__fastcall*)(void* pListBox, void* pEdx, void* pFocusable, uint32_t nUser, int nIndex, int nChannel);

static constexpr int nHighlightKeyboard = 1;
static constexpr int nHighlightPointer  = 0;

// The engine's other lookup: it scans a container's child vector rather than the document name
// table, which is the only way to reach anything that is not a value widget.
//
// The name is a std::string, built and destroyed with the game's own pair rather than hand-rolled.
// The buffer is at +04h and the object carries an allocator at +00h that both touch.
using FindArea_t    = void* (__stdcall*)(void* pContainer, void* pName);
using StringMake_t  = void* (__fastcall*)(void* pString, void* pEdx, const char* pText);
using StringFree_t  = void  (__fastcall*)(void* pString, void* pEdx);

static Crc32_t             Crc32             = nullptr;
static NameLookup_t        NameLookup        = nullptr;
static SetSelection_t      SetSelection      = nullptr;
static SetHighlight_t      SetHighlight      = nullptr;

// Every widget reaches its input handler through its own Focusable, which arrives as the sender.
// The row list's is caught the first time it is asked to do anything and kept for the times the
// keys arrive somewhere else.
static void*    pRowListFocusable = nullptr;

// An ordinary row's cached appearance, taken from a real row so a heading can be given it back.
static uint32_t nOrdinaryItemState = 0;
static bool     bOrdinaryItemState = false;

static SafetyHookInline RefreshRowHook{};
static SafetyHookInline TextDrawHook{};
static SafetyHookInline TextDrawPlainHook{};
static SafetyHookInline ImageDrawHook{};
static SafetyHookInline DrawRowHook{};
static SafetyHookInline ListDrawHook{};

// Which input device the last event came from, so the light can be moved with the same one.
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

// Resolved by offset and proved by its own first bytes. Dunia loads at its preferred base, so the
// offset is the address; the signatures carry no absolute immediates and would still match if it
// did not. The check guards against a different build of the DLL.
// The offset names the function and the signature proves it. When the offset does not check out,
// which is every one of these on a build the offsets were not taken from, the signature is asked to
// name the function on its own - and only when it is the one thing in the image that answers to it,
// since several of these bodies repeat.
static void* ResolveEngineFunction(uint32_t nOffset, const char* pSignature)
{
    if (hDunia == nullptr)
        return nullptr;

    auto pCode = reinterpret_cast<uint8_t*>(hDunia) + nOffset;
    if (IsReadable(pCode, 32) && MatchesSignature(pCode, pSignature))
        return pCode;

    auto scanned = dunia_pattern(pSignature);
    return scanned.size() == 1 ? scanned.get_first() : nullptr;
}

// Anything that fails to resolve leaves its part of the page where the layout put it, rather than
// half-corrected.
static void ResolveLayoutSupport()
{
    Crc32      = reinterpret_cast<Crc32_t>     (ResolveEngineFunction(0x00AA7150, "80 3D ? ? ? ? 00 0F 84 96 00 00 00 C6 05"));
    NameLookup = reinterpret_cast<NameLookup_t>(ResolveEngineFunction(ByBuild<uint32_t>(0x00AD2B30, 0x00AC2040), "56 8B 71 08 57 8B 7C 24 0C 33 D2 EB 03 8D 49 00"));

    SetHighlight = reinterpret_cast<SetHighlight_t>(ResolveEngineFunction(0x00A9D1A0,
        "56 8B F1 8B 86 D0 00 00 00 57 8B 7C 24 14 3B F8 89 BE D0 00"));

    SetSelection = reinterpret_cast<SetSelection_t>(ResolveEngineFunction(0x00A9C800,
        "51 53 56 8B F1 8B 8E 94 00 00 00 57 8B BE D4 00 00 00 85 FF"));

    FindArea   = reinterpret_cast<FindArea_t>  (ResolveEngineFunction(ByBuild<uint32_t>(0x00535B70, 0x00527C10), "8B 44 24 08 8B 54 24 04 56 50 52 E8 ? ? ? ? 8B F0 85 F6"));
    StringMake = reinterpret_cast<StringMake_t>(ResolveEngineFunction(ByBuild<uint32_t>(0x000BD1D0, 0x000BC530), "51 56 8B F1 33 C0 57 8D 4C 24 0B 88 44 24 0B 89 46 14 8D 46"));
    StringFree = reinterpret_cast<StringFree_t>(ResolveEngineFunction(0x000BCF90, "51 56 8B F1 83 7E 18 10 72 0D 8B 46 04 50 FF 15 ? ? ? ?"));
}

// Guarded on the document rather than repeated on every open: a document is keyed by page-name hash
// across the process and outlives a menu session, so the moves persist with it.
static void* pPreparedDocument = nullptr;

// The resolution the engine performs, repeated here so a name becomes an element without building a
// std::string for the engine's own lookup.
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

// The three state classes a moveable thing might carry. +26h is y on one family and the right edge
// on the other, so the class is read rather than assumed and an unrecognised one is left alone.
// The GOG build's .rdata sits 0x889D0 lower, so every magma vtable moves with it.
static uint32_t RectStateVTable()  { return ByBuild<uint32_t>(0x00EE6D74, 0x00E5E3A4); }
static uint32_t PosStateVTable()   { return ByBuild<uint32_t>(0x00EEBD24, 0x00E63354); }
static uint32_t ScaleStateVTable() { return ByBuild<uint32_t>(0x00EEA17C, 0x00E617AC); }
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
static void MoveInstanceUp(void* pInstance, int nUp)
{
    auto pState = SafeRead<void*>(pInstance, nWidgetState);
    if (pInstance == nullptr || pState == nullptr)
        return;

    auto nVTable = SafeRead<uint32_t>(pState, 0);
    auto nBase = reinterpret_cast<uint32_t>(hDunia);
    auto& nLock = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pInstance) + nWidgetLockMask);

    if (nVTable == nBase + RectStateVTable())
    {
        // Both edges by the same amount. Moving one stretches the box.
        nLock |= nLockRectY;
        auto pTop    = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(pState) + nStateRectTop);
        auto pBottom = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(pState) + nStateRectBottom);
        *pTop    = static_cast<int16_t>(*pTop - nUp);
        *pBottom = static_cast<int16_t>(*pBottom - nUp);
    }
    else if (nVTable == nBase + PosStateVTable() || nVTable == nBase + ScaleStateVTable())
    {
        nLock |= nLockPosY;
        auto pY = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(pState) + nStateRectRight);
        *pY = static_cast<int16_t>(*pY - nUp);
    }
    // Any other state class is left alone rather than written to on a guess.
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

// The title is a magma::Text with a RectState: top and bottom rather than a y, and both have to
// move or the box grows instead.
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

// Guarded on the document for the reason given at pPreparedDocument.
static void PrepareLayout(void* pPage)
{
    auto pDocument = SafeRead<void*>(pPage, nPageDocument);
    if (pDocument == nullptr || pDocument == pPreparedDocument)
        return;

    // Every line the layout gives, asked for by name. The first eight ship with options.mgb, the
    // rest exist only because the patch above put them there, so the list stops at the first name
    // that does not resolve. Without the patch the page runs on eight rows.
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
        RowLines[i] = { LineNames[i], true };
        nRowLines++;
    }

    // The second line carries the lone slider. The package patch repoints it at the arrows
    // template, so it is only unusable when that patch did not happen.
    if (nRowLines <= std::size(StockLines) && nRowLines > 1)
        RowLines[1].bUsable = false;

    // The lift lives in the layout, on the containers these sit inside, so the only reason to touch
    // a value widget at run time is to park it off the page when the line it belongs to carries no
    // value. Its own y is kept here for putting it back.
    for (size_t i = 0; i < nRowLines; i++)
    {
        LineWidgets[i] = pRowWidgets[i];
        LineY[i] = nRowY[i];
    }

    // The heading, the rule under it and the row labels are children of p_menu_nav and are drawn
    // relative to it, so moving the panel moves all three in step. Chasing the rule on its own
    // moved something named a_title_bar and changed nothing on screen. The value widgets hang off
    // the page's own content instead and are lifted by the package patch.
    auto pPanel = FindAreaByName(pDocument, szNavPanel);
    if (pPanel != nullptr)
    {
        MoveInstanceUp(pPanel, nLiftUnits);
    }
    else
    {
        // No panel: move what can be reached on its own and accept that the rule stays put.
        if (auto pTitleBar = FindAreaByName(pDocument, szTitleBar))
            MoveInstanceUp(pTitleBar, nLiftUnits);
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
// The settings store keeps floats and ints in one variant, so a row's integer traffic is converted
// at the edges.

static int SettingToRow(const MenuRow& row)
{
    if (row.nPref == Pref::COUNT)
        return 0;
    if (row.nValue == VALUE_FLOAT)
        return static_cast<int>(JackalFixSettings.GetFloat(row.nPref) * row.fScale + 0.5f);
    return JackalFixSettings.GetInt(row.nPref);
}

// Settings the menu applies but does not write. The four FOV values are the ones most likely to
// have been set by hand, and a hand-set number is rarely one a row can offer: 91.31 sits between 90
// and 95. Writing the row's choice would replace it with no way back, so these are applied for the
// session and the file is left alone.
static bool IsSessionOnly(Pref nPref)
{
    switch (nPref)
    {
    case PREF_FIELDOFVIEW:
    case PREF_VIEWMODELFIELDOFVIEW:
    case PREF_IRONSIGHTFIELDOFVIEW:
    case PREF_VEHICLEFIELDOFVIEW:

    // The same for the render resolution pair, so the written pair stays what a hundred per cent
    // means. Writing them would make each move the base for the next.
    case PREF_INTERNALRESOLUTIONX:
    case PREF_INTERNALRESOLUTIONY:
        return true;
    default:
        return false;
    }
}

// Whether a row can do anything as the game currently stands.
//
// The three Beyond Ultra rows edit the ultrahigh geometry, shadow and terrain quality blocks, and
// the game only reads the block for the level chosen in its own display options. With Shadow on
// High, Beyond Ultra Shadows moves and changes nothing, so it is greyed. Each row answers for its
// own block. renderconfig gates its writes on the same three answers, so a greyed row is one that
// is not being applied either.
//
// Unavailable is not off. The ini value is untouched, the row still shows what it holds, and it
// comes back when the game's own setting is raised.
static bool IsRowAvailable(const MenuRow& row)
{
    switch (row.nPref)
    {
    case PREF_BEYONDULTRAGEOMETRY: return JackalFixGeometryIsUltra();
    case PREF_BEYONDULTRASHADOWS:  return JackalFixShadowsAreUltra();
    case PREF_BEYONDULTRATERRAIN:  return JackalFixTerrainIsUltra();

    // The shipped ini has a [Debug] section with nothing under it, so these are greyed until a key
    // is written by hand. A network session locks all six whatever the file says, matching what
    // debug does for the length of the match.
    case PREF_DEBUGINVINCIBILITY:
    case PREF_DEBUGINFINITEAMMO:
    case PREF_DEBUGUNLOCKALLWEAPONS:
    case PREF_DEBUGNOCLIP:
    case PREF_DEBUGFREECAM:
    case PREF_DEBUGDIAMONDS:
        return !JackalFixInMultiplayer() && JackalFixSettings.IsInFile(row.nPref);

    default:                       return true;
    }
}

// The three answers as one value, so a page can tell the ground moved under it while it was built.
static uint32_t BeyondUltraAvailability()
{
    return (JackalFixGeometryIsUltra() ? 1u : 0u)
        | (JackalFixShadowsAreUltra() ? 2u : 0u)
        | (JackalFixTerrainIsUltra() ? 4u : 0u);
}

// SettingToRow against the file rather than what is in force. The two part company the moment a
// session-only row moves, and this is the number that has to stay on the ladder: nothing writes it
// back, so it is the only way to reach what the player wrote.
static int FileToRow(const MenuRow& row)
{
    if (row.nValue == VALUE_FLOAT)
        return static_cast<int>(JackalFixSettings.GetFileFloat(row.nPref) * row.fScale + 0.5f);
    return JackalFixSettings.GetFileInt(row.nPref);
}

// The value the row shipped with, or the setting as it stands if the mod declares no default.
static int DefaultForRow(const MenuRow& row)
{
    for (const auto& entry : PrefDefaults)
    {
        if (entry.nPref == row.nPref)
            return entry.nValue;
    }
    return SettingToRow(row);
}

// Matched by value rather than position. The ini may hold something the list does not offer, and
// the first entry stands in for it.
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
// A range row is drawn with arrows over a list of steps built as the row is built. A value the
// steps miss is inserted in its place, so a FieldOfView of 91.31 shows as 91.35 rather than
// snapping to the nearest step and rewriting a value nobody touched.
//
// The engine deep copies both arrays, so this storage only has to survive the call. Kept per line
// anyway, which costs little and makes the lifetime obvious.

static constexpr size_t nMaxSteps = 48;
// The longest step text any row produces is the render resolution's "200% (15360x8640)".
static constexpr size_t nStepTextLength = 24;

static int            StepValues[nMaxLines][nMaxSteps]{};
static const wchar_t* StepLabels[nMaxLines][nMaxSteps]{};
static wchar_t        StepText  [nMaxLines][nMaxSteps][nStepTextLength]{};

// Leaves a row holding the one choice it already has, so the arrows have nowhere to go. Reuses the
// per line step storage, which the row APIs deep copy out of anyway. A value not in its own list is
// left alone, there being nothing to put in its place.
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

    // Two values belong on the ladder without being on it. One is what the row is set to, since
    // dropping it would rewrite the ini on the next Apply. The other is what the file says, which
    // for a session-only row differs the moment the row moves and is the only way back to the
    // hand-written number. Both are placed in order among the steps, and a duplicate of a step or
    // of each other is shown once.
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

    // Everything loose below the step about to be drawn, then anything equal to it, which the walk
    // draws next.
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

    // Above the top of the ladder, or the ladder ran out of steps.
    while (nPlaced < nLoose && nCount < nMaxSteps)
        Emit(Loose[nPlaced++]);

    return nCount;
}

// ------------------------------------------------------------------------------------------------
// Reading and writing a row's value.
//
// The two control classes disagree on the width of the value, so each is asked in its own terms. A
// null widget means the name lookup failed when the row was built, and the getter would hand back
// an uninitialised field, so that case reports failure rather than a plausible looking zero.

// Where a row's colour lives depends on the class. An ImageState keeps four at +44h, one per
// corner, the same place the controller prompt glyphs are tinted. A TextState does not: magma::Text
// reads its colour from State+10h and treats State+44h as the shadow colour behind enable bytes at
// +48h and +49h, so four dwords at +44h on a Text overwrote the shadow, both flags and four bytes
// past the end of the object. Twice. The offset is chosen by class and the write stays inside it.
static constexpr ptrdiff_t nImageStateColour = 0x44;
static constexpr int nImageStateColourCount = 4;
static constexpr ptrdiff_t nTextStateColour = 0x10;

// Which magma class an offset is true of, by vtable. Nothing is read or written until the object
// has said what it is.
// Both classes sit 0x889D0 lower in the GOG build, which recompiled the same source.
static ptrdiff_t ImageVtableRva() { return ByBuild<ptrdiff_t>(0xEE6A04, 0xE5E034); }
static ptrdiff_t TextVtableRva() { return ByBuild<ptrdiff_t>(0xEE63E4, 0xE5DA14); }

// Fading a row's value, by which row is being drawn rather than by which object it is.
//
// The enabled flag handed to the row APIs only reaches the entry: Dunia+81D660 passes it to
// AddEntry and builds the value control separately, so the label greys and the value beside it
// stays black. Colouring the value object directly does not work either: the list keeps one set of
// row drawables and re-poses it per row, so a colour written into one belongs to every row.
//
// What tells the rows apart is which row the list is drawing. Its Draw at Dunia+A9F590 loops over
// its visible cells calling Dunia+A9EC60 with the row index as the second argument. The line is
// inherited rather than cleared by any list drawn inside a row, because a value on this page is
// itself a CListBox running the same Dunia+A9EC60 one level down.
//
// An unavailable row's label arrives at 32000000, black at an alpha of 32h, while the value beside
// it is FF000000 and the arrows are C8000000. The difference is the alpha alone, so the alpha is
// lowered to the label's and the colour left as it is. A ceiling rather than an assignment: the
// row's background strips are authored at alpha zero, and an opaque grey over them turned every
// unavailable row into a white band.
static int nDrawingLine = -1;

// The alpha the engine greys a disabled label to, read off an unavailable row's own Text. A row's
// parts are brought down to it and no further, so a value already fainter keeps what it had.
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

// magma::Text draws its string through one of two methods and the row's value goes through the
// plain one. Text::Draw, vtable slot 15 at Dunia+A95500, calls +DCh at Dunia+AB4D20 whenever the
// text has no scale of its own and nothing to lay out; only the turned or measured case reaches
// +D8h, Dunia+AB4350. Hooking +D8h alone saw none of these rows. Both read State+10h, both hooked.
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

// A line whose row is there but cannot be used. The only thing that fades.
static bool IsLineUnavailable(int nLine)
{
    if (nLine < 0 || nRowLines == 0 || nLine >= static_cast<int>(nRowLines))
        return false;

    const auto nSlot = nCurrentPage * nRowLines + static_cast<size_t>(nLine);
    const auto* pRow = nSlot < nSlots ? Slots[nSlot] : nullptr;
    return pRow != nullptr && pRow->nKind != ROW_HEADING && !IsRowAvailable(*pRow);
}

// Lent for one draw and taken straight back. The arrows are one pair of magma::Images for the whole
// list, re-posed per row, so a fade left on them would grey the next usable row drawn after an
// unavailable one. The write is only ever live for the one call that reads it.
struct FadedState
{
    uint8_t*  pState = nullptr;
    ptrdiff_t nOffset = 0;
    int       nCount = 0;
    uint32_t  Saved[4]{};
};

// Writes the faded colour into a widget's State on its way into that widget's own draw, and only
// once the object has agreed it is the class the offset belongs to.
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

static bool IsRowBound(void* pControl)
{
    if (pControl == nullptr)
        return false;

    return SafeRead<void*>(pControl, nControlNamedWidget) != nullptr;
}

static bool ReadRowValue(const MenuRow& row, void* pControl, int& nOut)
{
    if (!IsRowBound(pControl))
        return false;

    auto ppVTable = *reinterpret_cast<uintptr_t**>(pControl);
    auto pValue = reinterpret_cast<GetRowValue_t>(ppVTable[nRowGetValueSlot])(pControl, nullptr);
    if (pValue == nullptr)
        return false;

    // The two-value control stores its choices as single bytes; the multi-value one uses ints.
    auto nValue = row.nKind == ROW_BOOL
        ? static_cast<int>(*static_cast<const uint8_t*>(pValue))
        : *static_cast<const int*>(pValue);

    // Neither getter reports failure. Each returns a pointer into the control: to a scratch field
    // on success, and to a separate, never initialised field when the widget holds no valid
    // selection. So a plausible looking integer is not evidence of anything, and a value outside
    // what the row was built with is rejected rather than written to the user's ini.
    switch (row.nKind)
    {
    case ROW_BOOL:
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
        // A range row may legitimately carry a value from outside its steps, because the ini is
        // allowed to. Only reject what the row was never offered.
        if (nValue < row.nMinimum || nValue > row.nMaximum)
            return false;
        break;

    default:
        return false;
    }

    nOut = nValue;
    return true;
}

static void WriteRowValue(const MenuRow& row, void* pControl, int nValue)
{
    if (!IsRowBound(pControl))
        return;

    auto ppVTable = *reinterpret_cast<uintptr_t**>(pControl);
    auto pSet = reinterpret_cast<SetRowValue_t>(ppVTable[nRowSetValueSlot]);

    if (row.nKind == ROW_BOOL)
    {
        // Matched by value, not index: the control scans its byte array for one that compares
        // equal. A value it does not hold is a silent no-op.
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
// Ours is derived from the cloned class's record, so it inherits that ancestry and an is-a test
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

// Kept so an untouched widget can be told from one already taken over, and so a widget of another
// class is never handed a list's table.
static uintptr_t* pStockListVTable = nullptr;
static void* pJackalFixList = nullptr;

// ------------------------------------------------------------------------------------------------
// The overrides our clone carries. Everything else is stock behaviour.

static uint32_t* __fastcall JackalFixGetClassRecord(void*, void*)
{
    return JackalFixClassRecord;
}

// Captures what was moved on the page about to be torn down. Clear-rows deletes the controls, the
// delegates and the map behind them, so a value living only in a control is about to be lost.
static void CapturePendingValues()
{
    for (size_t i = 0; i < nBuiltRows; i++)
    {
        const auto* pRow = Slots[BuiltRows[i].nSlot];
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
        const auto* pRow = Slots[i];
        PendingValues[i] = pRow != nullptr ? SettingToRow(*pRow) : 0;
    }
    bPendingValid = true;
}

static void BuildRows(void* pPage)
{
    nBuiltRows = 0;
    nBuiltAvailability = BeyondUltraAvailability();

    // The row APIs look their widget template up in the page's document without checking there is
    // one, so a missing document is a null dereference several frames down.
    auto pDocument = *reinterpret_cast<void**>(static_cast<uint8_t*>(pPage) + nPageDocument);
    if (pDocument == nullptr)
        return;

    if (nCurrentPage >= nPages)
        nCurrentPage = 0;

    auto nFirst = nCurrentPage * nRowLines;

    for (size_t nLine = 0; nLine < nRowLines; nLine++)
    {
        auto nSlot = nFirst + nLine;
        const auto* pRow = nSlot < nSlots ? Slots[nSlot] : nullptr;

        // A line shows its arrows only when something is bound to them. A heading has no value and
        // a blank has no row, and a live < > against either reads as a setting that lost its
        // value, so the widget is parked until a value row lands on the line again.
        auto bHasValue = pRow != nullptr && pRow->nKind != ROW_HEADING;
        MoveRowWidget(LineWidgets[nLine], bHasValue ? LineY[nLine] : nParkedY);

        // A blank keeps the labels level with the widgets they pair to. Disabled, so the navigation
        // steps over it rather than parking on nothing.
        if (pRow == nullptr)
        {
            AddEntry(pPage, nullptr, L"", 0, nullptr);
            continue;
        }

        // A heading is a label and nothing else, added enabled: the only way to tell the engine a
        // row cannot be selected is the flag that also greys it, and a section title in the
        // disabled ink reads as a setting turned off. The navigation keeps the selection off it
        // instead.
        if (pRow->nKind == ROW_HEADING)
        {
            AddEntry(pPage, nullptr, pRow->pLabel, 1, nullptr);
            continue;
        }

        const auto* pWidgetName = RowLines[nLine].pWidgetName;
        auto nValue = PendingValues[nSlot];
        void* pControl = nullptr;

        // Greying and unselectable being one flag is what a row that can do nothing wants. Marking
        // the item afterwards is not the same: the ink is decided as the row is built.
        const auto bAvailable = IsRowAvailable(*pRow);
        const auto nEnabled = bAvailable ? 1 : 0;

        // No delegate on a value row: it changes in place rather than opening a page. That is also
        // what greys Accept while one is selected, as on Game Options.
        switch (pRow->nKind)
        {
        case ROW_BOOL:
            pControl = AddValueRow(pPage, nullptr, pRow->pLabel, szRowTemplate, pWidgetName, L"On", L"Off", nEnabled, nullptr);
            break;

        // An enumeration brings its own list, a range one generated for it. Otherwise the same row.
        case ROW_ENUM:
        case ROW_RANGE:
        {
            uint32_t nCount;
            const int* pValues;
            const wchar_t* const* pLabels;

            if (pRow->nKind == ROW_ENUM)
            {
                nValue = NearestEnumValue(*pRow, nValue);
                nCount = pRow->nValueCount;
                pValues = pRow->pValues;
                pLabels = pRow->pValueLabels;
            }
            else
            {
                nCount = BuildSteps(nLine, *pRow, nValue);
                pValues = StepValues[nLine];
                pLabels = StepLabels[nLine];
            }

            // A row that cannot do anything is built holding the one choice it has. The enabled
            // flag greys it and takes it off the selection but does not stop a click landing on an
            // arrow, and the engine walks whatever list it was handed.
            if (!bAvailable)
                nCount = CollapseToCurrent(nLine, nValue, pValues, pLabels, nCount);

            pControl = AddMultiValueRow(pPage, nullptr, pRow->pLabel, szRowTemplate, pWidgetName,
                                        nCount, pLabels, pValues, nEnabled, nullptr);
            break;
        }

        default:
            break;
        }

        if (pControl == nullptr)
            continue;

        PendingValues[nSlot] = nValue;
        BuiltRows[nBuiltRows++] = { nSlot, pControl };
        WriteRowValue(*pRow, pControl, nValue);
    }

    // Headings were added as ordinary rows so they draw in the ordinary ink. Marking them
    // unselectable now and putting the cached appearance back to a real row's costs nothing
    // visually. Done to the items because AddEntry sets both at once.
    if (auto ppItems = SafeRead<uint8_t**>(pPage != nullptr ? SafeRead<void*>(pPage, nPageList) : nullptr, nListBoxItems))
    {
        for (size_t nLine = 0; nLine < nRowLines && !bOrdinaryItemState; nLine++)
        {
            auto nSlot = nFirst + nLine;
            const auto* pRow = nSlot < nSlots ? Slots[nSlot] : nullptr;

            // Copying from a greyed row would put every heading into the disabled ink.
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
            const auto* pRow = nSlot < nSlots ? Slots[nSlot] : nullptr;
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

// The option layer's own bookkeeping, not ours. See the byte at page+1B8h above.
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

// Lights or greys one of the prompts along the bottom, in the numbering the base open uses.
static void SetActionPrompt(void* pPage, int nPrompt, bool bEnabled)
{
    if (pPage == nullptr || DispatcherSlot == nullptr || SetPromptEnabled == nullptr || nEventDispatcher == 0)
        return;

    if (auto pSlot = DispatcherSlot(static_cast<uint8_t*>(pPage) + nEventDispatcher, nullptr, nPrompt))
        SetPromptEnabled(pSlot, nullptr, bEnabled ? 1 : 0);
}

// Tears the window down and puts the next one up. Clear-rows deletes every control, every delegate
// and every node of the row map, and empties the value widgets each control had bound, so nothing
// accumulates across a page turn.
static void ShowPage(void* pPage, size_t nPage, bool bKeepOnScreenValues = true, int nTurnDirection = 0)
{
    if (nPage >= nPages)
        return;

    // What is on screen is normally the newest truth and is taken before the rows are destroyed.
    // Not when the caller has just decided what every row should say: DEFAULT does that, and
    // reading the old controls back over it is why pressing it appeared to do nothing.
    if (bKeepOnScreenValues)
        CapturePendingValues();

    auto nPrevious = nCurrentPage;
    auto bSameWindow = nPage == nCurrentPage;
    auto pList = SafeRead<void*>(pPage, nPageList);

    // Where the selection was. A rebuild that is not a page turn, DEFAULT or the ini changing
    // underneath, puts it back on the same line rather than at one end.
    auto nWasOn = SafeRead<int32_t>(pList, nListBoxSelected);

    // The option layer's state goes with the rows, so the one bit worth keeping is taken first.
    auto bChanged = IsPageChanged(pPage);

    auto ppVTable = *reinterpret_cast<uintptr_t**>(pPage);
    reinterpret_cast<PageMethod_t>(ppVTable[nClearRowsSlot])(pPage, nullptr);

    nCurrentPage = nPage;
    BuildRows(pPage);

    // Clearing the rows destroyed the controls and with them the option layer's subscription to
    // every row's change event, the only thing that lights APPLY. The base open plants those
    // subscriptions, so it runs again. It also declares the page unchanged and greys APPLY, which
    // is right for an open and wrong for a rebuild, so that much is put back.
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

    // Coming down onto a page starts at the top, coming up onto one at the bottom. A rebuild of
    // the window already on screen is not a turn and resumes where it left off.
    //
    // The page order gives the direction except when the list wraps: the last page turning down
    // onto the first is a step down even though the number went backwards. So the caller says which
    // way where it knows, and the order answers where it does not.
    auto bDownwards = nTurnDirection != 0 ? nTurnDirection > 0 : nPage > nPrevious;
    auto bResume = bSameWindow && nWasOn >= 0 && nWasOn < static_cast<int>(nRowLines);
    auto nFrom = bResume ? nWasOn : (bDownwards ? 0 : static_cast<int>(nRowLines) - 1);

    pList = SafeRead<void*>(pPage, nPageList);
    SettleSelection(pList, nFrom, bResume || bDownwards ? 1 : -1);

    // The lit bar is a separate field and does not follow the selection, so a rebuild in place
    // moves it by hand or leaves the light on whatever line the old rows had it.
    if (bResume && SetHighlight != nullptr && pRowListFocusable != nullptr && pList != nullptr)
    {
        auto nNow = SafeRead<int32_t>(pList, nListBoxSelected);
        if (nNow >= 0)
            SetHighlight(pList, nullptr, pRowListFocusable, nInputUser, nNow, nHighlightKeyboard);
    }

    // Clearing the list left the selection empty and the first row added takes it, so the
    // selection lands on the top line in both directions. Pressing up again from there turns
    // another page, which is what a paged list wants.
    if (pList != nullptr)
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pList) + nListBoxFirstVisible) = 0;
}

// Section titles are not drawn in an ink of their own. The rows share one template, so there is no
// per-row colour to write, and the missing fact is how to get from the row template to the State
// that carries its colour. Until that is read out of the disassembly rather than inferred, a
// cosmetic tint is not worth another crash.

// ------------------------------------------------------------------------------------------------
// The fix's mark on the front page.
//
// The front page draws one line of text from code, the version, and the mark is a second draw of
// the widget that draws it: the same object, the same ink and the same font, lent our own string
// and a box moved to the bottom right for the length of one call and put straight back.
//
// Which line that is, is decided by what is drawn rather than by who owns it. CFCXMainPage keeps
// the version at +198h, but what it keeps there is a magma::GenericObject, a handle the name table
// resolves rather than the thing that draws, and nothing reachable from it is the magma::Text these
// hooks are handed. The string is: "V %u.%02u" is built in code, on the front page alone.
//
// Both of magma::Text's string routines draw the mark after their own draw, since which of the two
// a widget goes through depends on state it carries. Neither re-enters: the second draw calls the
// trampoline, not the hook.

// The engine's std::wstring, as its own draw path reads it: the buffer at +04h is inline until
// eight characters and a pointer past that, the length at +14h and the capacity at +18h. Nothing on
// that path writes to it or frees it, so a literal of ours is lent through one with the capacity
// set past the inline limit, which is what makes +04h a pointer rather than the first characters.
static constexpr ptrdiff_t nWideBuffer   = 0x04;
static constexpr ptrdiff_t nWideLength   = 0x14;
static constexpr ptrdiff_t nWideCapacity = 0x18;
static constexpr size_t    nWideSize     = 0x1C;
static constexpr uint32_t  nWideInline   = 8;

struct GameWideString
{
    void*          pAllocator;
    const wchar_t* pText;
    uint32_t       Unused[3];
    uint32_t       nLength;
    uint32_t       nCapacity;
};

static const wchar_t szWatermarkText[] = L"Jackal Fix V1";

static GameWideString Watermark
{
    nullptr,
    szWatermarkText,
    {},
    static_cast<uint32_t>(std::size(szWatermarkText) - 1),
    0x100,
};

static const wchar_t* ReadWide(const void* pString, uint32_t& nLength)
{
    if (!IsReadable(pString, nWideSize))
        return nullptr;

    nLength = SafeRead<uint32_t>(pString, nWideLength);
    if (nLength == 0 || nLength > 512)
        return nullptr;

    const auto nCapacity = SafeRead<uint32_t>(pString, nWideCapacity);
    const auto* pText = nCapacity < nWideInline
        ? reinterpret_cast<const wchar_t*>(static_cast<const uint8_t*>(pString) + nWideBuffer)
        : SafeRead<const wchar_t*>(pString, nWideBuffer);

    return IsReadable(pText, (nLength + 1) * sizeof(wchar_t)) ? pText : nullptr;
}

static bool IsVersionLine(const void* pString)
{
    uint32_t nLength = 0;
    const auto* pText = ReadWide(pString, nLength);

    return pText != nullptr && nLength >= 4 && nLength <= 12
        && pText[0] == L'V' && pText[1] == L' ' && pText[2] >= L'0' && pText[2] <= L'9';
}

// magma::Text. +34h is the horizontal alignment its string routine reads, where two is flush right
// and is what keeps the mark on the same edge whatever the string measures. +50h is where the same
// routine records what it ran out of room for, so it is saved with the rest.
static constexpr ptrdiff_t nTextAlign      = 0x34;
static constexpr ptrdiff_t nTextDrawFlags  = 0x50;
static constexpr int32_t   nTextAlignRight = 2;

// magma::RectState. The widget's draw is translated to +24h and +28h before either string routine
// is entered, so writing those two moves nothing here: the translate is already on the stack by the
// time the routine we re-enter is called. What is still readable from inside it is +26h, the far
// edge the alignment measures against, and +34h, where the plain routine takes its line's y from.
static constexpr ptrdiff_t nStateRectLeft = 0x24;
static constexpr ptrdiff_t nStateTextY    = 0x34;

// So the mark is placed by the two levers the routine does read: the width of the box it aligns
// inside, which carries the right edge, and the line's y. Both constants below were measured off
// 720p captures rather than worked out, since the x the box is aligned inside is relative to a
// translate the parent chain pushed and nothing here can read.
//
// The version line sits at x 269, y 100 to 114 on a 720p capture. The mark mirrors that into the
// far corner: its right edge 269 px in from the right, and its bottom 130 px up from the bottom.
//
// Right edge, over three captures of the same string:
//     box 400 units wide                        right edge  627 px
//     box CanvasRight - 287 - left wide          right edge 1231 px
//     box CanvasRight - 522 - left wide          right edge 1017 px
// The middle to the last is 235 units for 214 px, so a unit is 0.911 px across here, and the 6 px
// still to come off the right is 7 more units.
//
// Drop, over the same three:
//     535 units   bottom 112 px to 594 px
//     564 units   bottom 619 px
// 29 units for 25 px, so a unit is about 0.87 px down here. 619 px is 29 px below the 590 px that
// 130 px up from the bottom asks for, which is 33 units back off the drop.
static constexpr int nWatermarkFromRight = 529;
static constexpr int nWatermarkDrop      = 531;

// The canvas right edge. Its height is the engine's own; its width is not stored anywhere the draw
// path reads, so it comes from the shape of the window the game is drawing into.
static int CanvasRight(int nBottom)
{
    RECT Client{};
    auto hWnd = JackalFixGameWindow();

    if (hWnd != nullptr && GetClientRect(hWnd, &Client)
        && Client.right > Client.left && Client.bottom > Client.top)
    {
        return nBottom * (Client.right - Client.left) / (Client.bottom - Client.top);
    }

    return nBottom * 16 / 9;
}

// The magma render state for this thread, whose +36h is the canvas bottom every text draw measures
// its lines down from.
using RenderState_t = void* (__cdecl*)();
static RenderState_t RenderState = nullptr;
static constexpr ptrdiff_t nRenderStateBottom = 0x36;

// The canvas bottom, or zero if the engine has not said anything believable.
static int16_t CanvasBottom()
{
    if (RenderState == nullptr)
        return 0;

    const auto nBottom = SafeRead<int16_t>(RenderState(), nRenderStateBottom);
    return nBottom > 100 && nBottom < 4096 ? nBottom : 0;
}

// The box keeps the left edge the layout gave it, since that is where the translate already put the
// line, and grows to the right until its far edge is where the mark should end. Flush right against
// that edge is what keeps the mark in place whatever the string measures.
//
// The y is the other lever, and each routine takes it from its own place: the laid out one from the
// argument it was called with, which the caller adjusts, and the plain one from the state, which is
// adjusted here.
template<typename Draw>
static void DrawWatermark(void* pText, Draw&& DrawString)
{
    const auto nBottom = CanvasBottom();
    if (nBottom == 0)
        return;

    auto pState = SafeRead<uint8_t*>(pText, nWidgetState);
    if (!IsReadable(pState, nStateTextY + sizeof(int16_t)))
        return;

    auto pLeft   = reinterpret_cast<int16_t*>(pState + nStateRectLeft);
    auto pLineY  = reinterpret_cast<int16_t*>(pState + nStateTextY);
    auto pAlign  = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pText) + nTextAlign);
    auto pFlags  = static_cast<uint8_t*>(pText) + nTextDrawFlags;

    const auto nSavedRight = pLeft[1];
    const auto nSavedLineY = *pLineY;
    const auto nSavedAlign = *pAlign;
    const auto nSavedFlags = *pFlags;

    // Measured from the canvas edge rather than from the line, so the mark keeps its distance from
    // the right of the screen at every aspect.
    const auto nWidth = CanvasRight(nBottom) - nWatermarkFromRight - pLeft[0];
    if (nWidth < 16)
        return;

    pLeft[1] = static_cast<int16_t>(pLeft[0] + nWidth);
    *pLineY  = static_cast<int16_t>(nSavedLineY - nWatermarkDrop);
    *pAlign  = nTextAlignRight;

    DrawString(&Watermark);

    pLeft[1] = nSavedRight;
    *pLineY  = nSavedLineY;
    *pAlign  = nSavedAlign;
    *pFlags  = nSavedFlags;
}

// The four draws the fade runs off. Both of magma::Text's string routines and magma::Image's draw
// read the row's colour first; which row that is comes from nDrawingLine, which the list's own draw
// sets and the per-cell draw overrides. Nothing here decides anything.
static void __fastcall JackalFixTextDraw(void* pText, void* pEdx, void* pArg1, void* pArg2)
{
    const auto faded = FadeForDraw(pText, TextVtableRva(), nTextStateColour, 1);
    TextDrawHook.fastcall(pText, pEdx, pArg1, pArg2);
    RestoreAfterDraw(faded);

    if (IsVersionLine(pArg1))
    {
        // The line's y, moved down by the same amount the plain routine's state y is.
        auto pDropped = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(pArg2) + nWatermarkDrop);
        DrawWatermark(pText, [&](void* pString) { TextDrawHook.fastcall(pText, pEdx, pString, pDropped); });
    }
}

static void __fastcall JackalFixTextDrawPlain(void* pText, void* pEdx, void* pArg)
{
    const auto faded = FadeForDraw(pText, TextVtableRva(), nTextStateColour, 1);
    TextDrawPlainHook.fastcall(pText, pEdx, pArg);
    RestoreAfterDraw(faded);

    if (IsVersionLine(pArg))
        DrawWatermark(pText, [&](void* pString) { TextDrawPlainHook.fastcall(pText, pEdx, pString); });
}

static void __fastcall JackalFixImageDraw(void* pImage, void* pEdx)
{
    const auto faded = FadeForDraw(pImage, ImageVtableRva(), nImageStateColour, nImageStateColourCount);
    ImageDrawHook.fastcall(pImage, pEdx);
    RestoreAfterDraw(faded);
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

// The other way in, catching what the per-row hook cannot. CListBox::Draw draws its cells between
// two widgets of its own, ListBox+58h before them and ListBox+74h after. The second is the
// highlight, drawn over the row under the pointer. Both are outside the cell loop, so the per-row
// hook never sees them and a faded arrow came back black the moment the pointer touched it.
//
// A value list caught here is on a line whether or not its own row is drawing it. The row list's
// draw is attributed to whichever line the light is on, ListBox+D0h; the per-row hook overrides
// that per cell and puts it back, leaving the two outside draws carrying it.
static void __fastcall JackalFixListDraw(void* pList, void* pEdx)
{
    const auto nPrevious = nDrawingLine;

    if (const auto nLine = pList != pJackalFixList ? ValueWidgetLine(pList) : -1; nLine >= 0)
        nDrawingLine = nLine;

    ListDrawHook.fastcall(pList, pEdx);
    nDrawingLine = nPrevious;
}

// Puts a heading's appearance back to an ordinary row's after the engine recomputes it. The
// disabled flag stays set and only the look is undone.
static void __fastcall JackalFixRefreshRow(void* pListBox, void* pEdx, int nIndex, int nAnimate)
{
    RefreshRowHook.fastcall(pListBox, pEdx, nIndex, nAnimate);

    if (pListBox != pJackalFixList || nIndex < 0 || nIndex >= static_cast<int>(nRowLines))
        return;

    auto nSlot = nCurrentPage * nRowLines + static_cast<size_t>(nIndex);
    const auto* pRow = nSlot < nSlots ? Slots[nSlot] : nullptr;
    if (pRow == nullptr)
        return;

    // Ordinary rows are left alone. The refresh puts a row's colours back where the artwork wants
    // them, so a value coloured here is undone on the next redraw. The fade waits for the draw.
    if (pRow->nKind != ROW_HEADING)
        return;

    if (!bOrdinaryItemState)
        return;

    auto ppItems = SafeRead<uint8_t**>(pListBox, nListBoxItems);
    auto pItem = ppItems != nullptr ? ppItems[nIndex] : nullptr;
    if (pItem != nullptr && IsReadable(pItem, nItemSize))
        *reinterpret_cast<uint32_t*>(pItem + nItemVisualState) = nOrdinaryItemState;
}

// A line the selection may rest on: a row with a value behind it, not a title or a spacer.
static bool IsSelectableLine(size_t nLine)
{
    auto nSlot = nCurrentPage * nRowLines + nLine;
    const auto* pRow = nSlot < nSlots ? Slots[nSlot] : nullptr;
    return pRow != nullptr && pRow->nKind != ROW_HEADING && IsRowAvailable(*pRow);
}

// The page the list moves to when the selection walks off one end of the window. The ends join,
// down from the last page is the first, and a single page answers itself, which both callers test
// for. Rows inside a window still step one at a time.
static size_t NextPage(int nDirection)
{
    if (nPages <= 1)
        return nCurrentPage;

    if (nDirection > 0)
        return nCurrentPage + 1 < nPages ? nCurrentPage + 1 : 0;

    return nCurrentPage > 0 ? nCurrentPage - 1 : nPages - 1;
}

// Puts the selection on the nearest line worth resting on, preferring the direction of travel. The
// fallback the other way is for the top of the first page, where a heading has nothing above it.
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

// Moves the selection one row, turning the page when it runs off the end. Done explicitly rather
// than by handing the key to the stock handler, which acts on the widget the input was routed to:
// with a value focused it was reached, detected the key and did nothing.
static bool StepRow(void* pPage, int nDirection)
{
    if (pJackalFixList == nullptr || SetSelection == nullptr || nRowLines == 0)
        return false;

    auto nSelected = SafeRead<int32_t>(pJackalFixList, nListBoxSelected);

    // A selection outside the window means the list is not where this thinks it is, not that the
    // page should turn. Start from just outside the near end so the first step lands inside.
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

    // Off the end of the window, so the window moves instead.
    auto nPage = NextPage(nDirection);
    if (nPage == nCurrentPage || pPage == nullptr)
        return false;

    ShowPage(pPage, nPage, true, nDirection);
    return true;
}

// The pointer can put the light on a section title by passing over it. The highlight is a separate
// field from the selection, so the navigation's heading skipping does not apply, and the light is
// moved on to the nearest real row however the pointer arrives.
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

// A value on a row that cannot be used takes no pointer at all, so the click never reaches the
// arrow and the value never takes the focus. The keyboard is already kept off by the row not being
// selectable.
static bool IsPointerRefused(void* pListBox)
{
    const auto nLine = ValueWidgetLine(pListBox);
    return nLine >= 0 && IsLineUnavailable(nLine);
}

// The three pointer entries do the same either side of their own stock method: refuse a row that
// cannot be used, remember the sender, keep the light off the section titles.
static int PointerEntry(ListBoxNav_t Stock, void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult)
{
    if (IsPointerRefused(pListBox))
        return 0;

    if (pListBox == pJackalFixList && pSender != nullptr)
        pRowListFocusable = pSender;

    auto nReturn = Stock(pListBox, pEdx, pSender, pEvent, pResult);
    KeepLightOffHeadings(pListBox, pSender, pEvent);
    return nReturn;
}

static int __fastcall JackalFixListBoxHover(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult)
{
    return PointerEntry(StockListBoxHover, pListBox, pEdx, pSender, pEvent, pResult);
}

static int __fastcall JackalFixListBoxPointer(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult)
{
    return PointerEntry(StockListBoxPointer, pListBox, pEdx, pSender, pEvent, pResult);
}

static int __fastcall JackalFixListBoxMouse(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult)
{
    if (IsPointerRefused(pListBox))
        return 0;

    auto nReturn = PointerEntry(StockListBoxMouse, pListBox, pEdx, pSender, pEvent, pResult);

    // Clicking a value leaves the keyboard pointed at it, so the row it belongs to is selected
    // again here. The light is not taken back: a value needs it to be worked with the mouse, and
    // taking it on the click stopped the arrows changing anything. See the input handler.
    auto nLine = ValueWidgetLine(pListBox);
    if (nLine >= 0)
    {
        nInputUser = SafeRead<uint8_t>(pEvent, 8);

        if (SetSelection != nullptr && pJackalFixList != nullptr)
            SetSelection(pJackalFixList, nullptr, nLine, 1, 1);
    }

    return nReturn;
}

// The list's own input handler, taken over for this page's list alone.
//
// The engine's version tries to move the selection and, failing, records the direction and asks for
// the input to be handed on. That refusal is the page turn, and wrapping is switched off so the
// refusal happens rather than the selection jumping back to the top of the same window.
//
// Rebuilding from here is safe. The handler caches no item pointer across the call it just made,
// re-reading the selection, the first visible index and the visible count afterwards, and the up
// and down path never runs through the row delegates that clear-rows deletes.
static int __fastcall JackalFixListBoxNav(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult)
{
    if (pListBox == pJackalFixList && pSender != nullptr)
        pRowListFocusable = pSender;
    nInputUser = SafeRead<uint8_t>(pEvent, 6);

    RefreshIfStale(pJackalFixPage);

    auto nFlagsBefore = pResult != nullptr ? pResult[nNavResultFlags] : 0;
    auto nReturn = StockListBoxNav(pListBox, pEdx, pSender, pEvent, pResult);

    // Clicking a value hands the keyboard to that value's own list, which only knows left and
    // right, so up and down went nowhere until something else was clicked. A value list that
    // refuses an up or a down passes it to the list of rows instead.
    if (pListBox != pJackalFixList && pResult != nullptr && ValueWidgetLine(pListBox) >= 0)
    {
        auto bRefusedHere = (pResult[nNavResultFlags] & nNavFlagUnhandled) != 0
                         && (nFlagsBefore & nNavFlagUnhandled) == 0;
        auto nCode = *reinterpret_cast<int32_t*>(pResult + nNavResultCode);

        if (!bRefusedHere || (nCode != nNavCodeUp && nCode != nNavCodeDown))
            return nReturn;

        // A value cannot use an up or a down, so the light goes back to the rows here, after the
        // click has had its use of it.
        if (SetHighlight != nullptr && pSender != nullptr)
        {
            SetHighlight(pListBox, nullptr, pSender, nInputUser, -1, nHighlightPointer);
            SetHighlight(pListBox, nullptr, pSender, nInputUser, -1, nHighlightKeyboard);
        }
        // The row is moved here rather than by passing the key on. The refusal is swallowed either
        // way, or the focus is handed to whatever sits beyond the page.
        StepRow(pJackalFixPage, nCode == nNavCodeDown ? 1 : -1);
        pResult[nNavResultFlags] = nFlagsBefore;
        return 1;
    }

    // The table is swapped per instance, so this should only ever be our own list. The check costs
    // a compare and guards against every list in the game turning pages.
    if (pListBox != pJackalFixList || pResult == nullptr || pJackalFixPage == nullptr || nPages <= 1)
        return nReturn;

    // Headings are ordinary selectable rows to the engine, so landing on one is stepped over by
    // moving again the same way. Bounded by the line count, or a window of nothing but headings
    // spins here.
    for (size_t nGuard = 0; nGuard < nRowLines; nGuard++)
    {
        if ((pResult[nNavResultFlags] & nNavFlagUnhandled) != 0)
            break;

        auto nSelected = SafeRead<int32_t>(pListBox, nListBoxSelected);
        if (nSelected < 0)
            break;

        // Headings and rows the game's own settings have made pointless are stepped over alike.
        if (IsSelectableLine(static_cast<size_t>(nSelected)))
            break;

        nReturn = StockListBoxNav(pListBox, pEdx, pSender, pEvent, pResult);
    }

    // Stepping over needs somewhere to step. At the ends of a page there is not, so the selection
    // is placed outright.
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

    // Keep the focus on the list. Without this the refusal just acted on also hands the key to
    // whatever sits above or below, and the selection leaves the page.
    pResult[nNavResultFlags] &= static_cast<uint8_t>(~nNavFlagUnhandled);
    return 1;
}

// Swapped per instance rather than hooked: the class is shared by every list in the game and only
// this one should turn pages. Repeated on every open in case the engine rebuilt the widget.
static void TakeOverList(void* pPage)
{
    auto pList = *reinterpret_cast<void**>(static_cast<uint8_t*>(pPage) + nPageList);
    if (pList == nullptr)
        return;

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

    // The value lists get the same table. They are this page's own widgets, and it is the only way
    // an up or a down pressed while one holds the focus reaches the rows.
    for (size_t i = 0; i < nRowLines; i++)
    {
        auto pWidget = LineWidgets[i];
        if (pWidget == nullptr || PointsIntoDunia(pWidget))
            continue;

        // Only a widget still carrying the stock list table. Another class is left alone rather
        // than handed a list's methods.
        if (pStockListVTable != nullptr && *reinterpret_cast<uintptr_t**>(pWidget) == pStockListVTable)
            *reinterpret_cast<uintptr_t**>(pWidget) = &JackalFixListVTable[1];
    }

    // One line per widget the layout carries, so the window matches the furniture, and no wrapping,
    // so a press past the last row is reported rather than swallowed.
    *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(pList) + nListBoxMaxVisible) = static_cast<uint8_t>(nRowLines);

    auto& nFlags = *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(pList) + nListBoxFlags);
    nFlags &= static_cast<uint8_t>(~nListBoxWrap);
}

// The page's own value-changed hook, raised on every arrow press with the row index that moved.
// CFCXOptionGamePage's version couples two rows together, hiding the machete row with difficulty,
// through row indices this clone never set. No row here depends on another, so doing nothing is
// correct rather than merely safe. The engine still updates the control's value before raising
// this, and the option layer's subscriber still lights the Apply prompt.
static void __fastcall JackalFixValueChanged(void*, void*, int)
{
}

// Apply, dispatched here by the option-page layer.
//
// Every row is written, not only the ones on screen, because a page turn destroys the controls that
// held the earlier choices and PendingValues carries them instead. A row is written only when it
// differs from the setting, compared against the setting rather than the row's starting value, so
// rounding rewrites nothing: a FieldOfView of 91.31 reads back as 91.35 and would otherwise be
// flattened on every Apply.
//
// Writing the ini is what makes a change take effect, since the plugin watches that file and
// re-reads it. onIniFileChange is fired here as well so the result does not depend on the watcher's
// timing, and because the watcher is only installed when the ini existed at start-up.

// True while this page is the one writing, so the reaction to the ini changing can tell its own
// work from somebody else's.
static bool bApplyingOurselves = false;

// Set when the ini has moved and the rows on screen no longer match it. Acted on from the engine's
// thread, never the watcher's.
static bool bRowsStale = false;

// Redraws the page if something changed it from outside. Only called from an engine callback.
static void RefreshIfStale(void* pPage)
{
    if (!bRowsStale)
        return;

    bRowsStale = false;

    if (pPage != nullptr && nBuiltRows != 0)
    {
        // Without the capture. The values came from the file the moment it changed, and reading
        // the controls back over them would undo the edit this exists to show.
        ShowPage(pPage, nCurrentPage, false);
    }
}

// Writes every staged value that differs from the setting and tells the rest of the plugin. Shared
// by APPLY and DEFAULT, which differ only in where the staged values came from.
static void CommitPendingValues()
{
    CIniReader iniReader("");
    bApplyingOurselves = true;

    for (size_t i = 0; i < nSlots; i++)
    {
        const auto* pRow = Slots[i];
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
    }

    // Fired whether or not anything moved. Everything that reacts to a setting hangs off this, so
    // firing it is what makes a change take hold in the running game.
    JackalFix::onIniFileChange().executeAll();
    bApplyingOurselves = false;
}

static void __fastcall JackalFixApplyPage(void*, void*)
{
    CapturePendingValues();
    CommitPendingValues();
}

// Revert, reached by the DEFAULT prompt through the option layer's confirmation box. Puts every row
// on every page back to the value the mod ships with, writes them, and redraws the window.
//
// Writing here rather than staging for APPLY is not a shortcut. The option layer greys both prompts
// and declares the page unchanged the instant this returns, since the stock page's DEFAULT commits
// on the spot, so anything left staged could never be committed at all.
static void __fastcall JackalFixDefaultPage(void* pPage, void*)
{
    for (size_t i = 0; i < nSlots; i++)
    {
        const auto* pRow = Slots[i];
        if (pRow == nullptr || pRow->nKind == ROW_HEADING || pRow->nPref == Pref::COUNT)
            continue;

        // An unavailable row is the one place a staged value can differ from the setting untouched,
        // and DEFAULT would then write a greyed row's ini key. Left holding what it holds.
        if (!IsRowAvailable(*pRow))
            continue;

        PendingValues[i] = DefaultForRow(*pRow);
    }

    bPendingValid = true;
    CommitPendingValues();

    // Without the capture, or the rows would be read back over the values just decided.
    if (pPage != nullptr)
        ShowPage(pPage, nCurrentPage, false);
}

// Events one, two and three are Apply, Revert and Back. The stock handler vtables are used as they
// are: each is a three instruction thunk onto the option layer's generic handler, which dispatches
// through the page's own Apply and Revert slots, both owned by the clone. CFCXOptionGamePage's
// versions of those slots were what made them unsafe, not the handlers.
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
        // mirrors the stock page rather than piling up handlers.
        if (auto pSlot = DispatcherSlot(pDispatcher, nullptr, static_cast<int>(i + 1)))
            RegisterHandler(pSlot, nullptr, pHandler);
    }
}

// Stands in for CFCXOptionGamePage::Open, which builds the stock game rows, clears a field and tail
// calls the base. This does the same with its own rows, so everything the base open expects is set
// up, including its subscription of the option layer's change handler to every row in the map,
// which is what lights the Apply prompt when a value moves.
static void __fastcall JackalFixOpenPage(void* pPage, void* pEdx)
{
    auto pBytes = static_cast<uint8_t*>(pPage);
    auto ppVTable = *reinterpret_cast<uintptr_t**>(pPage);

    // CListMenuPage::Open makes the same test before it touches anything. The base open does not,
    // and walking its control map on a page the engine has not brought up yet faults.
    if (*(pBytes + nPageReady) == 0 || *reinterpret_cast<void**>(pBytes + nPageDocument) == nullptr)
        return;

    // The line table and the moves settle before the slot plan is built against them: the plan is a
    // function of how many lines there are and what each one can hold.
    PrepareLayout(pPage);

    // Measured on every open, because the window it measures is one of the things this page can
    // change.
    SettleRenderResolutionChoices();

    PlanSlots();

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

    // On the first open of a session the Beyond Ultra rows are built against an answer that is not
    // settled: the profile the engine hands out before the base open has run is a preset template
    // reading ultrahigh for all three, and the live one appears only once it has. So the question
    // is asked again on the way out and the rows rebuilt if the answer moved. Once per session.
    if (BeyondUltraAvailability() != nBuiltAvailability)
    {
        ShowPage(pPage, nCurrentPage, false);
    }

    // After the base open rather than before. The base puts the selection on the first row as it
    // enters, so settling beforehand is undone a moment later and the page opens on a heading.
    SettleSelection(SafeRead<void*>(pPage, nPageList), 0, 1);
    bRowsStale = false;
}

// ------------------------------------------------------------------------------------------------

// Replaces the page heading. The stock title is inline or on the heap depending on its length, so
// both cases are handled and the existing buffer reused when it is big enough.
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
// of its own rather than sharing the real page's. The name reaches the constructor as a push of a
// string literal, so the swap repoints those immediates for the duration of the call. Only pushes
// whose target is the stock name are touched.
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
        return false;

    for (size_t i = 0; i < nSites; i++)
        injector::WriteMemory<uint32_t>(pSites[i], reinterpret_cast<uint32_t>(szJackalFixPageLayout), true);

    PageConstruct(pPage);

    for (size_t i = 0; i < nSites; i++)
        injector::WriteMemory<uint32_t>(pSites[i], nOriginals[i], true);

    return true;
}

// The Game row's delegate on the OPTIONS list carries CFCXOptionGamePage's class id, which is what
// its page creator pushes too, so one immediate out of BuildEntries identifies both the class to
// clone and the creator that says how to build it.
//
// This runs at start-up because it needs only the address of the class id, a link-time constant.
// Deriving the class record cannot: the creator fills that in lazily and it is all zeros at init.
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

    return PageConstruct != nullptr;
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

// Runs once the creator has filled the record in on its way past.
static bool ResolveClassRecord()
{
    if (pJackalFixClassId != nullptr)
        return true;

    auto pRecord = FindClassRecord(pClonedClassId);
    if (pRecord == nullptr)
        return false;

    // Same shape as the cloned class's record with one more link on the chain. The engine would put
    // a hash of the class name in that last slot; only the registry reads it, so anything unique
    // does. The address of our own name string is unique by construction and can never be the
    // 0xFFFFFFFF that marks a row as having no page behind it.
    JackalFixClassRecord[0] = reinterpret_cast<uint32_t>(szJackalFixPageClass);
    JackalFixClassRecord[1] = pRecord[1] + 1;
    for (uint32_t i = 0; i < pRecord[1]; i++)
        JackalFixClassRecord[2 + i] = pRecord[2 + i];
    JackalFixClassRecord[pRecord[1] + 2] = reinterpret_cast<uint32_t>(szJackalFixPageClass);

    pJackalFixClassId = &JackalFixClassRecord[JackalFixClassRecord[1] + 1];
    return true;
}

// The registry is rebuilt along with the menu, so this is asked every time rather than cached: a
// page registered against a previous manager is gone.
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
        return false;

    if (!ConstructWithOwnLayout(pPage))
        return false;

    if (JackalFixVTable[1] == 0)
    {
        // Copied from one slot before the table so the RTTI pointer that sits there comes with it.
        auto pSource = *reinterpret_cast<uintptr_t**>(pPage) - 1;
        for (size_t i = 0; i <= nVTableSlots; i++)
            JackalFixVTable[i] = pSource[i];

        // The stock open is three statements: build the rows, clear a field, tail call the base.
        // The field's offset and the base entry point are read out of it, and a shape that does
        // not match aborts instead of guessing.
        auto pOpen = reinterpret_cast<uint8_t*>(JackalFixVTable[1 + nOpenSlot]);
        if (pOpen[nOpenBuildRows] != 0xE8 || pOpen[nOpenBaseTailJump] != 0xE9)
            return false;

        nPageResetField = *reinterpret_cast<int32_t*>(pOpen + nOpenResetFieldDisp);
        BasePageOpen = reinterpret_cast<PageMethod_t>(RelativeTarget(pOpen + nOpenBaseTailJump, 1));

        // The prompt bookkeeping, read out of the base open. See the note beside the offsets. A
        // shape that does not match leaves both null and the page still works, losing only the
        // lighting of APPLY across a rebuild.
        auto pBase = reinterpret_cast<uint8_t*>(BasePageOpen);
        if (pBase[nBaseOpenDirtyOpcode] == 0xC6 && pBase[nBaseOpenDirtyOpcode + 1] == 0x87
            && pBase[nBaseOpenPromptSlot] == 0xE8 && pBase[nBaseOpenSetEnabled] == 0xE8)
        {
            nPageChanged     = *reinterpret_cast<int32_t*>(pBase + nBaseOpenDirtyDisp);
            SetPromptEnabled = reinterpret_cast<SetPromptEnabled_t>(CallTarget(pBase + nBaseOpenSetEnabled));
        }

        // The builder being replaced is the call this method opens with. It is never run, but its
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
        return false;

    *ppSlot = pPage;
    pJackalFixPage = pPage;
    return true;
}

// ------------------------------------------------------------------------------------------------

static SafetyHookInline BuildEntriesHook{};
static SafetyHookInline PageCreatorHook{};

// Built in the same pass the engine builds its own rather than later on demand. A page registered
// after the manager's start-up pass never gets its document loaded or its ready flag set, and
// opening it walks null. Created here off the same manager and parent, it initialises like any
// other.
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

    // The row goes in either way. A missing page adds it greyed rather than skipping it, so "no
    // row at all" and "row that does nothing" stay distinguishable symptoms.
    auto pManager = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pPage) + nPageManager);

    void* pDelegate = nullptr;
    if (HasJackalFixPage(pManager))
    {
        if (auto pStorage = GameAlloc(nDelegateSize, 0))
            pDelegate = MakeDelegate(pStorage, nullptr, pPage, pJackalFixClassId, 0, 1);
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

            // CFCXOptionPage::BuildEntries, anchored on the first sub page's delegate allocation.
            // The 7Ch size and the guard test after it are unique to this function.
            auto buildPattern = dunia_pattern("83 EC 40 53 55 56 57 33 DB 53 6A 7C 8B F1 E8 ? ? ? ? 8B F8 83 C4 08 3B FB 74 23");
            if (buildPattern.empty())
                return;

            // CListMenuPage::AddValueRow, the two-value row every options page uses. Anchored on
            // the 5Ch control allocation after the row append.
            auto rowPattern = dunia_pattern("51 8B 44 24 20 8B 54 24 1C 53 55 56 57 8B 7C 24 18 50 52 57 89 4C 24 1C E8 ? ? ? ? 33 DB 8B E8 53 6A 5C");
            if (rowPattern.empty())
                return;

            // AddMultiValueRow. Anchored on the 60h allocation and the vtable store after it.
            auto multiPattern = dunia_pattern("8B 44 24 20 53 55 8B 6C 24 0C 56 57 8B F9 8B 4C 24 2C 50 51 55 8B CF E8 ? ? ? ? 33 DB 53 6A 60");
            if (multiPattern.empty())
                return;

            // The page creation template, instantiated once per page class. Every instance shares
            // the same body, so any match resolves the registry helpers. The instance that builds
            // the cloned class is picked out later by the class id it pushes.
            PageCreators = dunia_pattern("51 83 3D ? ? ? ? 00 53 57 8B F9 75 07 33 C9 E8 ? ? ? ? 68 ? ? ? ? 8D 44 24 0C 8D 5F 04 50 8B CB E8 ? ? ? ? "
                                         "8B 44 24 08 3B 47 14 75 ? 56 6A 00 68 ? ? ? ? E8 ? ? ? ? 83 C4 08 85 C0 74 0B 8B C8 E8 ? ? ? ? 8B F0 EB 02 33 F6 "
                                         "8B 4C 24 14 89 BE 40 01 00 00 89 8E EC 00 00 00 83 7F 18 00 75 03 89 77 38 83 3D ? ? ? ? 00 75 07 33 C9 E8 ? ? ? ? "
                                         "68 ? ? ? ? 8B CB E8 ? ? ? ? 89 30");
            if (PageCreators.empty())
                return;

            pBuildEntries    = buildPattern.get_first<uint8_t>();
            AddValueRow      = reinterpret_cast<AddValueRow_t>(rowPattern.get_first<uint8_t>());
            AddMultiValueRow = reinterpret_cast<AddMultiValueRow_t>(multiPattern.get_first<uint8_t>());

            nDelegateSize = *reinterpret_cast<uint8_t*>(pBuildEntries + nBuildDelegateSize);
            MakeDelegate  = reinterpret_cast<MakeDelegate_t>(CallTarget(pBuildEntries + nBuildMakeDelegate));
            AddEntry      = reinterpret_cast<AddEntry_t>(CallTarget(pBuildEntries + nBuildAddEntry));

            // Any instance will do. The two registry helpers are shared by all of them.
            auto pCreator  = PageCreators.get(0).get<uint8_t>();
            RegistryFind   = reinterpret_cast<RegistryFind_t>(CallTarget(pCreator + nCreateRegistryFind));
            RegistryInsert = reinterpret_cast<RegistryInsert_t>(CallTarget(pCreator + nCreateRegistryInsert));

            // The slot plan is not built here. It is a function of how many lines the page ends up
            // with, which is not known until the document exists, first on opening the page.
            ResolveLayoutSupport();

            if (auto pRefreshRow = ResolveEngineFunction(0x00A9AE00,
                "56 57 8B 7C 24 0C 85 FF 8B F1 0F 8C ? ? ? ? 8B 8E 94 00"))
            {
                RefreshRowHook = safetyhook::create_inline(pRefreshRow, JackalFixRefreshRow);
            }

            if (auto pTextDraw = ResolveEngineFunction(0x00AB4350, szTextDrawPattern))
                TextDrawHook = safetyhook::create_inline(pTextDraw, JackalFixTextDraw);

            if (auto pTextDrawPlain = ResolveEngineFunction(0x00AB4D20, szTextDrawPlainPattern))
                TextDrawPlainHook = safetyhook::create_inline(pTextDrawPlain, JackalFixTextDrawPlain);

            if (auto pImageDraw = ResolveEngineFunction(0x00AB93F0, szImageDrawPattern))
                ImageDrawHook = safetyhook::create_inline(pImageDraw, JackalFixImageDraw);

            if (auto pDrawRow = ResolveEngineFunction(0x00A9EC60, szDrawRowPattern))
                DrawRowHook = safetyhook::create_inline(pDrawRow, JackalFixDrawRow);

            if (auto pListDraw = ResolveEngineFunction(0x00A9F590, szListDrawPattern))
                ListDrawHook = safetyhook::create_inline(pListDraw, JackalFixListDraw);

            // The mark on the front page. Without this the version line still draws; only the mark
            // after it is skipped.
            RenderState = reinterpret_cast<RenderState_t>(ResolveEngineFunction(0x004EE7F0,
                "8B 0D ? ? ? ? 56 8B 35 ? ? ? ? E8 ? ? ? ? 8B 44 B0 0C 5E C3"));

            auto pReaderRead = ResolveEngineFunction(0x00AE7BF0,
                "56 8B F1 83 7E 10 00 57 75 0D 8B 4E 18 8B 01 8B 50 10 FF D2 89 46 10");

            if (pReaderRead != nullptr)
                ReaderReadHook = safetyhook::create_inline(pReaderRead, ReaderRead);

            BuildEntriesHook = safetyhook::create_inline(pBuildEntries, BuildEntries);

            // The other half of keeping the rows and the file in step. The plugin watches the file
            // and re-reads it; this picks the new values up so an edit made outside the game shows
            // on a page already open. Our own writes are skipped, or applying would overwrite the
            // rows with what was just read back.
            JackalFix::onIniFileChange() += []()
            {
                if (bApplyingOurselves || nSlots == 0)
                    return;

                // Our own writes come back through the watcher a moment after applying them, by
                // which time bApplyingOurselves is long cleared, so the page rebuilt itself after
                // every APPLY. Compare rather than race the timing: if the file says what the rows
                // say, there is nothing to do.
                bool bDiffers = false;
                for (size_t i = 0; i < nSlots && !bDiffers; i++)
                {
                    const auto* pRow = Slots[i];
                    if (pRow == nullptr || pRow->nKind == ROW_HEADING || pRow->nPref == Pref::COUNT)
                        continue;

                    bDiffers = SettingToRow(*pRow) != PendingValues[i];
                }

                if (!bDiffers)
                    return;

                // Reading the settings into our own array is safe from any thread. Rebuilding the
                // rows is not: this runs on the file watcher's thread and tearing the rows down
                // while the game draws them faults inside the row draw. So the values are taken now
                // and the page is redrawn on the engine's thread, at the next key or the next open.
                ResetPendingValues();
                bRowsStale = true;

            };

            // Only the creator is resolved here. Its class record is still all zeros at this point,
            // filled in the first time the creator runs, so our own record waits until then.
            if (!ResolveCreator())
                return;

            PageCreatorHook = safetyhook::create_inline(pPageCreator, CreateGamePage);
        };
    }
} JackalFixMenu;
