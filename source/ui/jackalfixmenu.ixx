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
import borderless;    // game window, for measuring render resolution percentages
import renderconfig;  // the game's own quality presets
import debug;         // network session state, which locks the debug rows

static constexpr ptrdiff_t nPageList          = 0x0C;  // row list widget
static constexpr ptrdiff_t nPageDocument      = 0x14;  // page's MAGMA document
static constexpr ptrdiff_t nPageReady         = 0x164; // set once the page is fit to open
static constexpr ptrdiff_t nPageTitle         = 0xF4;  // std::wstring drawn as the page heading
static constexpr ptrdiff_t nPageTitleSize     = 0x104;
static constexpr ptrdiff_t nPageTitleCapacity = 0x108;
static constexpr ptrdiff_t nPageParent        = 0xEC;  // page the navbar's Cancel returns to
static constexpr ptrdiff_t nPageManager       = 0x140; // owning menu manager

static constexpr ptrdiff_t nManagerRegistry   = 0x04;  // class id -> page map
static constexpr ptrdiff_t nRegistryEnd       = 0x10;  // that map's end iterator; a miss returns it

// std::wstring leaves its inline buffer for the heap at eight characters.
static constexpr uint32_t nWideStringInlineCapacity = 7;

// magma::ListBox fields. Wrap cleared: the nav handler's refusal past the last row is the page-turn
// signal.
static constexpr ptrdiff_t nListBoxMaxVisible   = 0x18; // byte; visible lines = min(this, item count)
static constexpr ptrdiff_t nListBoxFlags        = 0x19; // bit 0 = wrap at the ends
static constexpr uint8_t   nListBoxWrap         = 0x01;
static constexpr ptrdiff_t nListBoxFirstVisible = 0xCC;
// +44h disabled flag, which SetSelection refuses; +38h the cached visual state. Separate, so set the
// flag and write an ordinary row's state back.
static constexpr ptrdiff_t nListBoxItems       = 0x94;
static constexpr size_t    nItemSize           = 0x54;
static constexpr ptrdiff_t nItemVisualState    = 0x38;
static constexpr ptrdiff_t nItemDisabled       = 0x44;

static constexpr ptrdiff_t nListBoxHighlight    = 0xD0;
static constexpr ptrdiff_t nListBoxSelected     = 0xD4;

// Per-instance vtable swap: every list shares the class. These four slots are every way a row lights.
static constexpr size_t nListBoxNavSlot    = 27; // +6Ch
static constexpr size_t nListBoxHoverSlot  = 28; // +70h, pointer moving over a row
static constexpr size_t nListBoxMouseSlot  = 29; // +74h, click
static constexpr size_t nListBoxPointerSlot = 30; // +78h, the other pointer entry

static constexpr size_t nListBoxVTableSlots = 45;

// Nav result when the selection could not move: direction code plus a pass-it-on flag. Swallow the
// flag or a page turn also moves focus off the list.
static constexpr ptrdiff_t nNavResultCode   = 0x10;
static constexpr ptrdiff_t nNavResultFlags  = 0x16;
static constexpr uint8_t   nNavFlagUnhandled = 0x04;
static constexpr int       nNavCodeUp        = 0;
static constexpr int       nNavCodeDown      = 1;

// Page vtable slots. 19, 20 and 21 must be owned by a clone: the stock versions read row indices at
// page+1D8h..1F8h, still -1 here, and operator[] then dereferences a null inserted against key -1.
static constexpr size_t nClassRecordSlot  = 1;
static constexpr size_t nOpenSlot         = 2;
static constexpr size_t nClearRowsSlot    = 16;
static constexpr size_t nValueChangedSlot = 19; // +4Ch
static constexpr size_t nApplySlot        = 20; // +50h
static constexpr size_t nRevertSlot       = 21; // +54h

// Copied wholesale. The real table is 26 entries; the extra slots are never dispatched.
static constexpr size_t nVTableSlots = 40;

// Byte offsets into the pattern-resolved functions.
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

// Apply, Revert and Back handlers, wired from the stock builder's prologue. Without them the first
// prompt press dispatches into null.
static constexpr ptrdiff_t nBuildHandlerSize     = 0x0E; // push 0Ch, the handler allocation size
static constexpr ptrdiff_t nBuildHandlerInit     = 0x22; // call <handler base ctor>
static constexpr ptrdiff_t nBuildDispatcherDisp  = 0x37; // disp32 of lea ebp,[esi+disp32]
static constexpr ptrdiff_t nBuildDispatcherSlot  = 0x3F; // call <slot for event id>
static constexpr ptrdiff_t nBuildRegisterHandler = 0x46; // call <register>
static constexpr ptrdiff_t nBuildHandlerVTables[]{ 0x29, 0x66, 0x9D }; // mov [edi], <vtable>

// The base open clears the changed byte at page+1B8h, greys APPLY and lights DEFAULT. That byte
// gates APPLY; our rebuild runs the same base open, so all three are read out of it.
static constexpr ptrdiff_t nBaseOpenDirtyOpcode = 0x1B; // mov byte ptr [edi+disp32], 0
static constexpr ptrdiff_t nBaseOpenDirtyDisp   = 0x1D;
static constexpr ptrdiff_t nBaseOpenPromptSlot  = 0x22; // call <slot for event id>
static constexpr ptrdiff_t nBaseOpenSetEnabled  = 0x29; // call <light or grey that prompt>

// Which prompt is which, in the numbering the dispatcher uses.
static constexpr int nPromptApply   = 1;
static constexpr int nPromptDefault = 2;

// Engine functions. __thiscall declared as __fastcall with an unused EDX.

using GameAlloc_t      = void*    (__cdecl*)(size_t nSize, int nUnused);
using PageConstruct_t  = void*    (__fastcall*)(void* pPage);
using PageMethod_t     = void     (__fastcall*)(void* pPage, void* pEdx);
using RegistryFind_t   = void     (__fastcall*)(void* pRegistry, void* pEdx, void** ppIterator, const uint32_t* pClassId);
using RegistryInsert_t = void**   (__fastcall*)(void* pRegistry, void* pEdx, const uint32_t* pClassId);
using MakeDelegate_t   = void*    (__fastcall*)(void* pDelegate, void* pEdx, void* pOwnerPage, const uint32_t* pClassId, int nUnused, char bSetParent);
using AddEntry_t       = int      (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pText, int bEnabled, void* pDelegate);

// Two-value row, the < Yes / No > arrows. Registers the control in a map at page+190h, so it needs
// the value-page base: a plain CListMenuPage is only 190h bytes.
using AddValueRow_t    = void*    (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pLabel, const char* pTemplate,
                                                const char* pWidgetName, const wchar_t* pOnText, const wchar_t* pOffText,
                                                int bEnabled, void* pDelegate);

// Arbitrary choice list. Both arrays are deep copied as the row is built (values to control+4Ch),
// so neither outlives the call. Still drawn as arrows.
using AddMultiValueRow_t = void*  (__fastcall*)(void* pPage, void* pEdx, const wchar_t* pLabel, const char* pTemplate,
                                                const char* pWidgetName, uint32_t nCount,
                                                const wchar_t* const* pLabels, const int* pValues,
                                                int bEnabled, void* pDelegate);

// Selects by matching value against the control's own list, not by index.
using SetRowValue_t    = void     (__fastcall*)(void* pControl, void* pEdx, const void* pValue);
using GetRowValue_t    = const void* (__fastcall*)(void* pControl, void* pEdx);
static constexpr size_t nRowSetValueSlot = 13;
static constexpr size_t nRowGetValueSlot = 14;

// magma::ListBox's input handler, taken over per instance. pResult is where it records a refused key.
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

// Two one-argument calls, not one two-argument call: reading it as the latter unbalances the stack
// by eight bytes.
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

// One template function per page class, so any instance gives the shared registry helpers; the one
// we clone is picked out by the class id it pushes.
static hook::pattern PageCreators{};
static uint8_t* pBuildEntries = nullptr;
static uint8_t* pPageCreator = nullptr;

// Clone CFCXOptionGamePage, not CFCXOptionPage: the row APIs need the value-page base. Documents are
// keyed by page-name hash, so we need a name of our own; the PC build never names the non-_PC one.
static const char szStockPageLayout[]     = "MAINMENU_OPTIONGAME_PAGE_PC";
static const char szJackalFixPageLayout[] = "MAINMENU_OPTIONGAME_PAGE";

static const wchar_t szJackalFixPageTitle[] = L"JACKAL FIX";

// Widget template each value row is stamped from.
static const char szRowTemplate[] = "SETTING_LABEL_LIST";

// A bind failure is silent: arrows draw with nothing between them.
static constexpr ptrdiff_t nControlNamedWidget = 0x44; // the widget a value row found by name

// How far into the constructor to look for the layout name.
static constexpr ptrdiff_t nCtorScanBytes = 0x600;

// Labels are dynamic rows; value widgets are fixed layout furniture, so row N pairs only with the
// widget on line N. The lone slider line is unusable until the package patch repoints it.
struct PageLine
{
    const char* pWidgetName;
    bool        bUsable;
};

// The eight the layout ships with. SETTING_9 and SETTING_10 are in the file but fail the name lookup.
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

// Title and row block lift, in MAGMA units; the layout is 1280x800, so a unit is an 800th of height.
static constexpr int    nLiftUnits   = 94;

// The stock eight plus the patched-in rows, with headroom.
static constexpr size_t nMaxLines = 24;

static PageLine RowLines[nMaxLines]{};
static size_t   nRowLines = 0;

// The value widget per line and its home y. Parked off screen while the line carries no value.
static void*   LineWidgets[nMaxLines]{};
static int16_t LineY[nMaxLines]{};

// The widget name per line; patched-in names are built at run time.
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

// Off the bottom of the page; parking changes nothing but position.
static constexpr int16_t nParkedY = 4000;

// A row's value widget is the innermost list, so its own y always reads 0: the per-row offset lives
// on the container above, which the name does not resolve to.

// The page's contents, declared per ini section and in ini order.

enum RowKind
{
    ROW_HEADING, // label with no control; its line's widget is parked
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

    // ROW_RANGE. The control is integral, so a float setting is fixed point: ini value times fScale,
    // converted at the edges. Min, max and step are in those units.
    int   nMinimum;
    int   nMaximum;
    int   nStep;
    float fScale;
    const wchar_t* pFormat; // how a step is written out, e.g. L"%d" or L"%.2f"

    // ROW_ENUM. Both arrays are deep copied as the row is built.
    const int*            pValues;
    const wchar_t* const* pValueLabels;
    uint32_t              nValueCount;

    // A second setting moved in step, chosen by the same index; a render resolution is two ini keys.
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

// Boolean whose ini value is a float: the two states write 0 and 1 as floats.
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

// Choice lists.

static const int     DisplayModeValues[]{ 1, 2, 3 };
static const wchar_t* const DisplayModeLabels[]{ L"Fullscreen", L"Borderless", L"Windowed" };

static const int     ScalingFilterValues[]{ 0, 1 };
static const wchar_t* const ScalingFilterLabels[]{ L"Point", L"Bilinear" };

// Percentages of InternalResolutionX/Y, both axes together to keep the aspect, each choice writing
// both keys. No "off": an unset pair already means the window.
static constexpr int nRenderScaleStep = 10;
static constexpr int nRenderScaleFloor = 20;
static constexpr int nRenderScaleCeiling = 200;

// The Xbox 360 cut, offered at 16:9. Not a percentage, so it heads the list as its own pair.
static constexpr int nConsoleWidth = 1280;
static constexpr int nConsoleHeight = 696;

// Smallest frame the engine copes with, and the largest square a D3D9 render target can describe.
static constexpr int nRenderPixelMinW = 320;
static constexpr int nRenderPixelMinH = 240;
static constexpr int nRenderPixelMax = 16384;

// Filled in at page open; the base pair is not known before. One entry per step.
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

// The sections, in the order the ini declares them.

// Not const: the render resolution row's ends are settled against the window at page open.
static MenuRow DisplayRows[]
{
    Heading(L"DISPLAY"),
    Enumeration(L"Display Mode", PREF_DISPLAYMODE, "Display", "DisplayMode", DisplayModeValues, DisplayModeLabels),
    // By hand, not through Enumeration: the choice count is settled at page open. Also the one row
    // carrying a second ini key.
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
    Boolean(L"No Sprint And Aim Blur", PREF_NOSPRINTAIMBLUR, "Graphics", "NoSprintAimBlur"),
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

// Hundredths and "%g": "%d" handed a double prints 0, and the ini may hold 91.35. Steps of five,
// with the ini's own value kept wherever it falls between two.
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

// The base is the file's InternalResolutionX/Y, not what is in force, or a session move becomes the
// next base. Unset means the window, measured: borderless and the video options disagree. Walks
// outwards from 100 to the first step outside the pixel limits.
static void SettleRenderResolutionChoices()
{
    auto nBaseW = JackalFixSettings.GetFileInt(PREF_INTERNALRESOLUTIONX);
    auto nBaseH = JackalFixSettings.GetFileInt(PREF_INTERNALRESOLUTIONY);

    // 100% of the window writes back as unset, so the pair keeps meaning "follow the window".
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

// What DEFAULT restores: the shipped ini values, in the row's own units (saturation 0.5 is 50).

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
    { PREF_NOSPRINTAIMBLUR,         0   },
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

// The slot plan: slot S draws on line S modulo the line count. Blanks fill skipped slots, and a
// heading is pushed to the next page rather than stranded on the last line of one.
static constexpr size_t nMaxSlots = 256;

// One entry per slot, null for a blank.
static const MenuRow* Slots[nMaxSlots]{};
static size_t   nSlots = 0;
static size_t   nPages = 0;

// Per slot, the value chosen but not yet applied: a page turn destroys the controls that hold it.
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

        // A heading needs only its first row to fit on the same page; requiring all of it would
        // push the heading down forever.
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

// What was built, so nothing has to ask the engine's row map: its operator[] inserts a null on a
// miss and hands it back.

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

// Inferred offsets are read through here: a wrong one costs a zero, not a crash.
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

// More rows for the layout, inserted into options.mgb on its way into the parser: nothing adds a
// ninth value widget at runtime. Sequential object graph, no absolute offsets or checksum, so only
// the enclosing counts need correcting. Any anchor that fails to resolve leaves the buffer alone.

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

// Nine clears the Back / Default / Apply prompts after the lift, and is all the page has room for.
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

// Steps over one { key, tag, payload }. Zero for an unknown tag: the payload length depends on it,
// so the walk has lost the stream.
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

// Where the patch has to write, found by walking rather than written down.
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

    // The page whose map names the settings rows; the name repeats, so the shape settles it.
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

    // Anchored on the names this page's own map points at; the type byte alone is not distinctive.
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

    // Other pages reuse these row names, so the block is found instead by a contiguous run of
    // records sharing one x that covers most of the map's rows.
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

// One row container in the file's own shape; every unnamed byte is zero in the originals. The
// constants are copied verbatim, meaning unknown, so the caller checks them against a real record.
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

// Builds the replacement image. Zero leaves the original bytes alone and the page runs on eight rows.
static uint32_t BuildPatchedPackage(const uint8_t* pFile, uint32_t nLength, uint8_t* pOut, uint32_t nOutCapacity)
{
    PackageSites Sites{};
    if (!FindPackageSites(pFile, nLength, Sites))
        return 0;

    // Prove a real row has this shape before emitting nine more like it.
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

        // Not a hash of the name alone; unique is all that is required of it.
        auto nId = Crc32Of(szRow) ^ 0x5A5A5A5Au;

        uint8_t Record[nRowRecordSize]{};
        MakeRowRecord(Record, szRow, Sites.nRowX,
            static_cast<uint16_t>(nExtraRowFirstY + nExtraRowStep * i - nLiftUnits), nId);
        Append(Record, nRowRecordSize);
    }

    Append(pFile + Sites.nRowsEnd, nLength - Sites.nRowsEnd);

    // Lifted here, not at run time: the container is what the mouse hit-tests, so moving the widget
    // alone left the arrows keyboard-only.
    for (auto nRow = Sites.nRowsBegin + nAddedMap; nRow < Sites.nRowsEnd + nAddedMap; nRow += nRowRecordSize)
    {
        auto nY = ReadU16(pOut, nRow + nRowY);
        WriteU16(pOut, nRow + nRowY, static_cast<uint16_t>(nY - nLiftUnits));
    }

    // Sixty-five pool sizes, each exact for the stock file, and the allocator pops a free list head
    // unchecked, so extra elements need headroom. Which pool is which is unknown, so all get it.
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

    // The child count sits after the map, so it has moved by the bytes the map grew by.
    WriteU32(pOut, Sites.nMapCount, ReadU32(pOut, Sites.nMapCount) + nRows);
    WriteU32(pOut, Sites.nChildCount + nAddedMap, ReadU32(pOut, Sites.nChildCount + nAddedMap) + nRows);

    // Repoint the lone slider line at the arrows template, map entry included, or it stays a blank.
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

// Hooked after the buffered reader's own read, when the whole file sits in one heap block. Safe to
// swap the block: the patch inserts well past the header, and the read position is left alone.

static constexpr ptrdiff_t nReaderBuffer   = 0x04;
static constexpr ptrdiff_t nReaderBuffered = 0x08;
static constexpr ptrdiff_t nReaderPosition = 0x0C;
static constexpr ptrdiff_t nReaderSize     = 0x10;

// Only a file that has barely been read is worth looking at, which is every file exactly once.
static constexpr uint32_t nReaderEarly = 32;

// Not the page allocator: this block is freed by the engine's own. Moved in the GOG build.
static uint32_t EngineAllocSlot() { return ByBuild<uint32_t>(0x00FB6440, 0x00F0F3F0); }

using EngineAlloc_t = void* (__cdecl*)(size_t nSize, int nFlags);

static SafetyHookInline ReaderReadHook{};

// Never cached: the slot holds a placeholder until the engine installs the real allocator.
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

// The first added row's name marks a patched buffer; correct even if the engine reloads an old one.
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

    // Handed over, not copied: the archive owns and frees it. Original leaked, one per menu build.
    *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pReader) + nReaderBuffer) = pPatched;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pReader) + nReaderSize) = nPatched;
}

// Reshaping the page. Position lives on the magma::State at widget+08h and the keyframe evaluator
// would undo a poke, so the lock mask at widget+0Ch is set to make the move permanent.

// magma::Widget.
static constexpr ptrdiff_t nWidgetState    = 0x08;
static constexpr ptrdiff_t nWidgetLockMask = 0x0C;
static constexpr uint32_t  nLockPositionY  = 0x00000C04; // pos.y, rect.top, rect.bottom

// magma::State. ScaleState (ListBox, AreaInstance): +24h x, +26h y. RectState (Text): +24h..+2Ah
// left, right, top, bottom. The difference is silent, so each widget is written in its own terms.
static constexpr ptrdiff_t nStateScaleY     = 0x26;
static constexpr ptrdiff_t nStateRectTop    = 0x28;
static constexpr ptrdiff_t nStateRectBottom = 0x2A;

// magma::Element, and the property container every named object hangs its name table from.
static constexpr ptrdiff_t nOwnerProperties   = 0x0C;
static constexpr ptrdiff_t nElementWidget     = 0x14;

// The page's title text widget.
static constexpr ptrdiff_t nPageTitleWidget   = 0x10;

// magma::FullLink, the payload of a tag-12h name-table entry: dwords alternating node pointers and
// path hashes. The name resolves to index 2 * ((count - 1) / 2).
static constexpr ptrdiff_t nFullLinkBegin    = 0x08;
static constexpr ptrdiff_t nFullLinkEnd      = 0x0C;
static constexpr ptrdiff_t nVariantTag       = 0x04;
static constexpr ptrdiff_t nVariantPayload   = 0x08;
static constexpr int32_t   nVariantLinkTag   = 0x12;

// The panel holding heading, rule and row list, and the bar the heading shares with its rule. The
// name table holds only value widgets, so both are reached by walking a parent's children.
static const char szNavPanel[] = "p_menu_nav";
static const char szTitleBar[] = "a_title_bar";

using Crc32_t             = uint32_t (__cdecl*)(uint32_t* pOut, const char* pText);
using NameLookup_t        = void*    (__fastcall*)(void* pContainer, void* pEdx, uint32_t nCrc);

// Moves the selection, refreshing the ink at either end.
using SetSelection_t      = int      (__fastcall*)(void* pListBox, void* pEdx, int nIndex, int bRefresh, int bScroll);

// The highlight is not the selection: +D4h selected, +D0h lit, set by different calls. This is the
// call that lights a row; index -1 releases it.
using SetHighlight_t      = void     (__fastcall*)(void* pListBox, void* pEdx, void* pFocusable, uint32_t nUser, int nIndex, int nChannel);

static constexpr int nHighlightKeyboard = 1;
static constexpr int nHighlightPointer  = 0;

// Scans a container's child vector, the only way to reach a non-value widget. Its name argument is
// a std::string, built and destroyed with the game's own pair.
using FindArea_t    = void* (__stdcall*)(void* pContainer, void* pName);
using StringMake_t  = void* (__fastcall*)(void* pString, void* pEdx, const char* pText);
using StringFree_t  = void  (__fastcall*)(void* pString, void* pEdx);

static Crc32_t             Crc32             = nullptr;
static NameLookup_t        NameLookup        = nullptr;
static SetSelection_t      SetSelection      = nullptr;
static SetHighlight_t      SetHighlight      = nullptr;

// The row list's Focusable, caught off the first sender and kept for when keys arrive elsewhere.
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

// Offset first, Dunia loading at its preferred base, and proved by its own first bytes. Otherwise
// the signature, and only on a sole match, several of these bodies repeating.
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

// A failure leaves that part of the page where the layout put it.
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

// Guarded on the document: it is keyed by page-name hash and outlives a menu session, moves and all.
static void* pPreparedDocument = nullptr;

// The engine's own resolution, repeated here to avoid building a std::string for its lookup.
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

// +26h is y on one state family and the right edge on the other, so the class is read, not assumed,
// and an unrecognised one is left alone. GOG's .rdata sits 0x889D0 lower, magma vtables with it.
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
    // Any other state class is left alone.
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

// The title is a magma::Text with a RectState: move top and bottom together or the box grows.
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

static void PrepareLayout(void* pPage)
{
    auto pDocument = SafeRead<void*>(pPage, nPageDocument);
    if (pDocument == nullptr || pDocument == pPreparedDocument)
        return;

    // Every line, asked for by name. Stops at the first that does not resolve; without the package
    // patch that is the eight options.mgb ships with.
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

    // Line two carries the slider, unusable only where the package patch did not happen.
    if (nRowLines <= std::size(StockLines) && nRowLines > 1)
        RowLines[1].bUsable = false;

    // The lift lives in the layout, so a value widget is only touched to park it. Its own y is kept
    // here for putting it back.
    for (size_t i = 0; i < nRowLines; i++)
    {
        LineWidgets[i] = pRowWidgets[i];
        LineY[i] = nRowY[i];
    }

    // Heading, rule and row labels are children of p_menu_nav, so the panel moves all three. Value
    // widgets hang off the page's own content and are lifted by the package patch.
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

// The settings store keeps floats and ints in one variant; a row is integral, so convert at the edges.

static int SettingToRow(const MenuRow& row)
{
    if (row.nPref == Pref::COUNT)
        return 0;
    if (row.nValue == VALUE_FLOAT)
        return static_cast<int>(JackalFixSettings.GetFloat(row.nPref) * row.fScale + 0.5f);
    return JackalFixSettings.GetInt(row.nPref);
}

// Applied for the session, never written: a hand-set FOV is rarely one a row can offer, and writing
// the row's choice would replace it with no way back.
static bool IsSessionOnly(Pref nPref)
{
    switch (nPref)
    {
    case PREF_FIELDOFVIEW:
    case PREF_VIEWMODELFIELDOFVIEW:
    case PREF_IRONSIGHTFIELDOFVIEW:
    case PREF_VEHICLEFIELDOFVIEW:

    // Same for the render pair, or each move becomes the base for the next.
    case PREF_INTERNALRESOLUTIONX:
    case PREF_INTERNALRESOLUTIONY:
        return true;
    default:
        return false;
    }
}

// The Beyond Ultra rows edit the ultrahigh blocks, which the game reads only at that quality level,
// so each is greyed below it. Unavailable is not off: the ini value is untouched.
static bool IsRowAvailable(const MenuRow& row)
{
    switch (row.nPref)
    {
    case PREF_BEYONDULTRAGEOMETRY: return JackalFixGeometryIsUltra();
    case PREF_BEYONDULTRASHADOWS:  return JackalFixShadowsAreUltra();
    case PREF_BEYONDULTRATERRAIN:  return JackalFixTerrainIsUltra();

    // [Debug] ships empty, so these are greyed until a key is written by hand; a network session
    // locks all six whatever the file says.
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

// SettingToRow against the file, the only way back once a session-only row has moved.
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

// Matched by value: the ini may hold something the list does not offer, and entry zero stands in.
static int NearestEnumValue(const MenuRow& row, int nValue)
{
    for (uint32_t i = 0; i < row.nValueCount; i++)
    {
        if (row.pValues[i] == nValue)
            return nValue;
    }
    return row.nValueCount != 0 ? row.pValues[0] : nValue;
}

// Generated step lists. A value the steps miss is inserted rather than snapped, which would rewrite
// a value nobody touched. Deep copied by the engine, so this need only survive the call.

static constexpr size_t nMaxSteps = 48;
// The longest step text any row produces is the render resolution's "200% (15360x8640)".
static constexpr size_t nStepTextLength = 24;

static int            StepValues[nMaxLines][nMaxSteps]{};
static const wchar_t* StepLabels[nMaxLines][nMaxSteps]{};
static wchar_t        StepText  [nMaxLines][nMaxSteps][nStepTextLength]{};

// Leaves a row holding the one choice it has, so the arrows have nowhere to go. A value not in its
// own list is left alone.
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

    // The current value and the file's belong on the ladder without being on it: placed in order
    // among the steps, deduplicated.
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

    // Loose values below the next step, then swallow any equal to it.
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

// The two control classes disagree on the width of a value. A null widget means the bind failed and
// the getter would hand back an uninitialised field, so that case reports failure.

// ImageState: four colours at +44h, one per corner. TextState: one at +10h, +44h being its shadow,
// so four dwords there ran past the end of the object. The offset is chosen by class.
static constexpr ptrdiff_t nImageStateColour = 0x44;
static constexpr int nImageStateColourCount = 4;
static constexpr ptrdiff_t nTextStateColour = 0x10;

// Class by vtable; nothing is read or written until the object has said what it is.
static ptrdiff_t ImageVtableRva() { return ByBuild<ptrdiff_t>(0xEE6A04, 0xE5E034); }
static ptrdiff_t TextVtableRva() { return ByBuild<ptrdiff_t>(0xEE63E4, 0xE5DA14); }

// Fading by which row is drawing, not which object: one set of drawables is re-posed per row, and
// the row index from the per-cell draw is what tells them apart. Alpha only, and as a ceiling: the
// background strips are authored at alpha zero and an opaque grey turns the row into a white band.
static int nDrawingLine = -1;

// The engine's own disabled-label alpha. A ceiling, so a value already fainter keeps what it had.
static constexpr uint32_t nFadedAlpha = 0x32;
static constexpr int nAlphaShift = 24;
static constexpr uint32_t nColourMask = 0x00FFFFFF;

// CListBox::Draw.
static const char* const szListDrawPattern =
    "83 EC 14 56 8B F1 8B 46 08 8B 48 10 C1 E9 18 84 C9 57 75 22";

// The per-cell draw, anchored on the items array it reads with the index it was handed.
static const char* const szDrawRowPattern =
    "83 EC 1C 53 8B D9 8B 8B 94 00 00 00 85 C9 57 89 5C 24 14 75 04";

// magma::Image::Draw, anchored on the load of the widget's State, where the four colours live.
static const char* const szImageDrawPattern =
    "83 EC 2C 53 8B 1D ? ? ? ? 56 57 8B F9 8B 0D ? ? ? ? 8B 77 08 E8";

// magma::Text has two string routines, laid-out and plain. Both read State+10h, so both are hooked.
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

// Lent for one draw: the arrows are one pair of magma::Images for the whole list, re-posed per row.
struct FadedState
{
    uint8_t*  pState = nullptr;
    ptrdiff_t nOffset = 0;
    int       nCount = 0;
    uint32_t  Saved[4]{};
};

// Fades on the way into a widget's own draw, once its class has agreed the offset.
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

    // Neither getter reports failure, so anything outside what the row was built with is rejected
    // rather than written to the user's ini.
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
        // A range row may legitimately hold a value off its steps; only the bounds are enforced.
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
        // Matched by value, not index. A value the control does not hold is a silent no-op.
        uint8_t nByte = nValue != 0 ? 1 : 0;
        pSet(pControl, nullptr, &nByte);
    }
    else
    {
        pSet(pControl, nullptr, &nValue);
    }
}

// The type record is { const char* name, uint32_t depth, uint32_t nameHash[depth] }, and a class is
// the address of the trailing slot, not the hash. Derived from the clone's, so is-a still succeeds.

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

// Tells an untouched widget from one taken over, and keeps another class off a list's table.
static uintptr_t* pStockListVTable = nullptr;
static void* pJackalFixList = nullptr;

// The overrides our clone carries. Everything else is stock behaviour.

static uint32_t* __fastcall JackalFixGetClassRecord(void*, void*)
{
    return JackalFixClassRecord;
}

// Clear-rows deletes the controls, so anything living only in one is taken first.
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

    // The row APIs use the document unchecked, so a missing one faults several frames down.
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

        // Arrows only where a value is bound; otherwise parked until a value row lands here again.
        auto bHasValue = pRow != nullptr && pRow->nKind != ROW_HEADING;
        MoveRowWidget(LineWidgets[nLine], bHasValue ? LineY[nLine] : nParkedY);

        // A blank keeps labels level with their widgets. Disabled, so navigation steps over it.
        if (pRow == nullptr)
        {
            AddEntry(pPage, nullptr, L"", 0, nullptr);
            continue;
        }

        // Added enabled: unselectable and greyed are one flag, and a greyed title reads as a
        // setting turned off. The navigation keeps the selection off it instead.
        if (pRow->nKind == ROW_HEADING)
        {
            AddEntry(pPage, nullptr, pRow->pLabel, 1, nullptr);
            continue;
        }

        const auto* pWidgetName = RowLines[nLine].pWidgetName;
        auto nValue = PendingValues[nSlot];
        void* pControl = nullptr;

        // Here that shared flag is wanted. The ink is decided as the row is built, so it goes in now.
        const auto bAvailable = IsRowAvailable(*pRow);
        const auto nEnabled = bAvailable ? 1 : 0;

        // No delegate: a value row changes in place, which also greys Accept while it is selected.
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

            // The enabled flag does not stop a click landing on an arrow, so a dead row is built
            // holding the one choice it has.
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

    // Headings went in as ordinary rows for the ink; mark them unselectable now and put the cached
    // appearance back, AddEntry having set both at once.
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

// Tears the window down and puts the next one up; clear-rows leaves nothing behind.
static void ShowPage(void* pPage, size_t nPage, bool bKeepOnScreenValues = true, int nTurnDirection = 0)
{
    if (nPage >= nPages)
        return;

    // The screen is normally the newest truth. Not after DEFAULT, which has already decided every
    // row; reading the old controls back over it is why pressing it appeared to do nothing.
    if (bKeepOnScreenValues)
        CapturePendingValues();

    auto nPrevious = nCurrentPage;
    auto bSameWindow = nPage == nCurrentPage;
    auto pList = SafeRead<void*>(pPage, nPageList);

    // A rebuild that is not a page turn puts the selection back on the same line, not at an end.
    auto nWasOn = SafeRead<int32_t>(pList, nListBoxSelected);

    // The option layer's state goes with the rows, so the one bit worth keeping is taken first.
    auto bChanged = IsPageChanged(pPage);

    auto ppVTable = *reinterpret_cast<uintptr_t**>(pPage);
    reinterpret_cast<PageMethod_t>(ppVTable[nClearRowsSlot])(pPage, nullptr);

    nCurrentPage = nPage;
    BuildRows(pPage);

    // Clearing the rows dropped the change subscriptions that light APPLY, so the base open runs
    // again. It also declares the page unchanged, which a rebuild has to undo.
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

    // Down onto a page starts at the top, up at the bottom; a rebuild in place resumes. Page order
    // gives the direction except on the wrap, so the caller says which way where it knows.
    auto bDownwards = nTurnDirection != 0 ? nTurnDirection > 0 : nPage > nPrevious;
    auto bResume = bSameWindow && nWasOn >= 0 && nWasOn < static_cast<int>(nRowLines);
    auto nFrom = bResume ? nWasOn : (bDownwards ? 0 : static_cast<int>(nRowLines) - 1);

    pList = SafeRead<void*>(pPage, nPageList);
    SettleSelection(pList, nFrom, bResume || bDownwards ? 1 : -1);

    // The lit bar does not follow the selection, so a rebuild in place moves it by hand.
    if (bResume && SetHighlight != nullptr && pRowListFocusable != nullptr && pList != nullptr)
    {
        auto nNow = SafeRead<int32_t>(pList, nListBoxSelected);
        if (nNow >= 0)
            SetHighlight(pList, nullptr, pRowListFocusable, nInputUser, nNow, nHighlightKeyboard);
    }

    // The first row added takes the selection, so the window goes back to the top to match.
    if (pList != nullptr)
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pList) + nListBoxFirstVisible) = 0;
}


// The front page mark: a second draw of the version line's own widget, lent our string and a box in
// the corner for one call. Identified by its "V %u.%02u"; the second draw calls the trampoline, so
// neither string routine re-enters.

// std::wstring as the draw path reads it: +04h inline buffer until eight characters then a pointer,
// +14h length, +18h capacity. Capacity past the inline limit is what makes +04h a pointer.
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

// magma::Text. +34h horizontal alignment, 2 being flush right. +50h what the routine ran out of
// room for, saved with the rest.
static constexpr ptrdiff_t nTextAlign      = 0x34;
static constexpr ptrdiff_t nTextDrawFlags  = 0x50;
static constexpr int32_t   nTextAlignRight = 2;

// magma::RectState. The translate is already on the stack by then, so only +26h, the far edge the
// alignment measures against, and +34h, the plain routine's line y, still move anything.
static constexpr ptrdiff_t nStateRectLeft = 0x24;
static constexpr ptrdiff_t nStateTextY    = 0x34;

// Measured off 720p captures; the x is relative to a translate nothing here can read.
static constexpr int nWatermarkFromRight = 529;
static constexpr int nWatermarkDrop      = 531;

// The canvas right edge: the width is nowhere the draw path reads, so it comes from the window shape.
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

// Per-thread magma render state; +36h is the canvas bottom every text draw measures down from.
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

// Keep the left edge, grow the right to where the mark ends, draw flush right. The y comes from the
// argument in the laid-out routine and from the state in the plain one, adjusted here.
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

    // From the canvas edge, not the line, so the distance holds at every aspect.
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

// The four draws the fade runs off; nDrawingLine says which row is drawing.
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

// Restored, not cleared: our own values are lists too, and clearing is what left them black.
static void __fastcall JackalFixDrawRow(void* pList, void* pEdx, int nIndex, void* pPosition)
{
    const auto nPrevious = nDrawingLine;

    if (pList == pJackalFixList)
        nDrawingLine = nIndex;

    DrawRowHook.fastcall(pList, pEdx, nIndex, pPosition);
    nDrawingLine = nPrevious;
}

// CListBox::Draw draws two widgets outside the cell loop, one being the highlight under the pointer,
// so a faded arrow came back black the moment the pointer touched it.
static void __fastcall JackalFixListDraw(void* pList, void* pEdx)
{
    const auto nPrevious = nDrawingLine;

    if (const auto nLine = pList != pJackalFixList ? ValueWidgetLine(pList) : -1; nLine >= 0)
        nDrawingLine = nLine;

    ListDrawHook.fastcall(pList, pEdx);
    nDrawingLine = nPrevious;
}

// Puts a heading's look back after the engine recomputes it; the disabled flag stays set.
static void __fastcall JackalFixRefreshRow(void* pListBox, void* pEdx, int nIndex, int nAnimate)
{
    RefreshRowHook.fastcall(pListBox, pEdx, nIndex, nAnimate);

    if (pListBox != pJackalFixList || nIndex < 0 || nIndex >= static_cast<int>(nRowLines))
        return;

    auto nSlot = nCurrentPage * nRowLines + static_cast<size_t>(nIndex);
    const auto* pRow = nSlot < nSlots ? Slots[nSlot] : nullptr;
    if (pRow == nullptr)
        return;

    // Ordinary rows are left alone: the refresh undoes any colour written here, so fading waits
    // for the draw.
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

// The page a walk off the end of the window lands on. The ends join; a single page answers itself.
static size_t NextPage(int nDirection)
{
    if (nPages <= 1)
        return nCurrentPage;

    if (nDirection > 0)
        return nCurrentPage + 1 < nPages ? nCurrentPage + 1 : 0;

    return nCurrentPage > 0 ? nCurrentPage - 1 : nPages - 1;
}

// The nearest restable line, preferring the direction of travel; the other way covers a leading
// heading with nothing above it.
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

// One row, turning the page at the end. Explicit: the stock handler acts on the widget the input
// was routed to, which with a value focused did nothing.
static bool StepRow(void* pPage, int nDirection)
{
    if (pJackalFixList == nullptr || SetSelection == nullptr || nRowLines == 0)
        return false;

    auto nSelected = SafeRead<int32_t>(pJackalFixList, nListBoxSelected);

    // A selection outside the window is not a page turn; start just outside the near end.
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

// The pointer lights a heading by passing over it: the highlight is a separate field, so the
// navigation's heading skipping does not reach it.
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

// An unavailable row's value takes no pointer at all, so a click never reaches its arrows.
static bool IsPointerRefused(void* pListBox)
{
    const auto nLine = ValueWidgetLine(pListBox);
    return nLine >= 0 && IsLineUnavailable(nLine);
}

// Common to the three pointer entries: refuse a dead row, keep the sender, light no headings.
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

    // Reselect the row the clicked value belongs to. The light stays on the value, or its arrows
    // stop changing anything.
    auto nLine = ValueWidgetLine(pListBox);
    if (nLine >= 0)
    {
        nInputUser = SafeRead<uint8_t>(pEvent, 8);

        if (SetSelection != nullptr && pJackalFixList != nullptr)
            SetSelection(pJackalFixList, nullptr, nLine, 1, 1);
    }

    return nReturn;
}

// The list's input handler, for this page's list alone. Its refusal to move the selection is the
// page-turn signal, which needs wrap off. Safe to rebuild from here: it caches nothing across the
// call, and the up and down path never runs through the row delegates clear-rows deletes.
static int __fastcall JackalFixListBoxNav(void* pListBox, void* pEdx, void* pSender, void* pEvent, uint8_t* pResult)
{
    if (pListBox == pJackalFixList && pSender != nullptr)
        pRowListFocusable = pSender;
    nInputUser = SafeRead<uint8_t>(pEvent, 6);

    RefreshIfStale(pJackalFixPage);

    auto nFlagsBefore = pResult != nullptr ? pResult[nNavResultFlags] : 0;
    auto nReturn = StockListBoxNav(pListBox, pEdx, pSender, pEvent, pResult);

    // A focused value list only knows left and right, so it passes up and down to the row list.
    if (pListBox != pJackalFixList && pResult != nullptr && ValueWidgetLine(pListBox) >= 0)
    {
        auto bRefusedHere = (pResult[nNavResultFlags] & nNavFlagUnhandled) != 0
                         && (nFlagsBefore & nNavFlagUnhandled) == 0;
        auto nCode = *reinterpret_cast<int32_t*>(pResult + nNavResultCode);

        if (!bRefusedHere || (nCode != nNavCodeUp && nCode != nNavCodeDown))
            return nReturn;

        // The light goes back to the rows now the click has had its use of it.
        if (SetHighlight != nullptr && pSender != nullptr)
        {
            SetHighlight(pListBox, nullptr, pSender, nInputUser, -1, nHighlightPointer);
            SetHighlight(pListBox, nullptr, pSender, nInputUser, -1, nHighlightKeyboard);
        }
        // Moved here, not by passing the key on; the refusal is swallowed either way.
        StepRow(pJackalFixPage, nCode == nNavCodeDown ? 1 : -1);
        pResult[nNavResultFlags] = nFlagsBefore;
        return 1;
    }

    // The table is swapped per instance, so this should only ever be our own list.
    if (pListBox != pJackalFixList || pResult == nullptr || pJackalFixPage == nullptr || nPages <= 1)
        return nReturn;

    // Step over a heading by moving again the same way; bounded, or a window of headings spins.
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

    // No room to step over at the ends of a page, so the selection is placed outright.
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

    // Swallow the refusal, or it also hands the key to whatever sits beyond the list.
    pResult[nNavResultFlags] &= static_cast<uint8_t>(~nNavFlagUnhandled);
    return 1;
}

// Per instance, not hooked: every list shares the class. Repeated per open in case of a rebuild.
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

    // Value lists get the same table; it is the only way a focused value passes up and down on.
    for (size_t i = 0; i < nRowLines; i++)
    {
        auto pWidget = LineWidgets[i];
        if (pWidget == nullptr || PointsIntoDunia(pWidget))
            continue;

        // Only widgets still carrying the stock list table; another class is left alone.
        if (pStockListVTable != nullptr && *reinterpret_cast<uintptr_t**>(pWidget) == pStockListVTable)
            *reinterpret_cast<uintptr_t**>(pWidget) = &JackalFixListVTable[1];
    }

    // Window matches the layout's line count; wrap off, so a press past the last row is reported.
    *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(pList) + nListBoxMaxVisible) = static_cast<uint8_t>(nRowLines);

    auto& nFlags = *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(pList) + nListBoxFlags);
    nFlags &= static_cast<uint8_t>(~nListBoxWrap);
}

// No row depends on another, so nothing to do; the engine still updates and still lights APPLY.
static void __fastcall JackalFixValueChanged(void*, void*, int)
{
}

// Apply. Every row, not only those on screen, and only where it differs from the setting so rounding
// rewrites nothing. onIniFileChange is fired here rather than left to the watcher's timing.

// True while this page is writing, so the ini reaction can tell its own work from somebody else's.
static bool bApplyingOurselves = false;

// Set when the ini has moved under the rows. Acted on from the engine's thread, never the watcher's.
static bool bRowsStale = false;

// Redraws the page if something changed it from outside. Only called from an engine callback.
static void RefreshIfStale(void* pPage)
{
    if (!bRowsStale)
        return;

    bRowsStale = false;

    if (pPage != nullptr && nBuiltRows != 0)
    {
        // No capture: reading the controls back would undo the edit this exists to show.
        ShowPage(pPage, nCurrentPage, false);
    }
}

// Shared by APPLY and DEFAULT, which differ only in where the staged values came from.
static void CommitPendingValues()
{
    CIniReader iniReader("");
    bApplyingOurselves = true;

    // mINI truncates and rewrites the whole file per key, so every write below opens a window in
    // which the watcher could read a half-written ini, default everything and broadcast that back.
    bJackalFixWritingIni = true;

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

    // The file is whole again; a watcher event still in flight now re-reads it intact.
    bJackalFixWritingIni = false;

    // Fired whether or not anything moved; everything that reacts to a setting hangs off it.
    JackalFix::onIniFileChange().executeAll();
    bApplyingOurselves = false;
}

static void __fastcall JackalFixApplyPage(void*, void*)
{
    CapturePendingValues();
    CommitPendingValues();
}

// Revert, behind the DEFAULT prompt. Written now rather than staged for APPLY: the option layer
// greys both prompts the instant this returns, so anything staged could never be committed.
static void __fastcall JackalFixDefaultPage(void* pPage, void*)
{
    for (size_t i = 0; i < nSlots; i++)
    {
        const auto* pRow = Slots[i];
        if (pRow == nullptr || pRow->nKind == ROW_HEADING || pRow->nPref == Pref::COUNT)
            continue;

        // Or DEFAULT writes a greyed row's ini key.
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

// Events 1, 2 and 3 are Apply, Revert and Back. The stock handler vtables are used as they are: each
// thunks onto the generic handler, which dispatches through the clone's own Apply and Revert slots.
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

        // Registering replaces and releases, so repeating this on every open piles nothing up.
        if (auto pSlot = DispatcherSlot(pDispatcher, nullptr, static_cast<int>(i + 1)))
            RegisterHandler(pSlot, nullptr, pHandler);
    }
}

// Stands in for CFCXOptionGamePage::Open: build the rows, clear a field, tail call the base.
static void __fastcall JackalFixOpenPage(void* pPage, void* pEdx)
{
    auto pBytes = static_cast<uint8_t*>(pPage);
    auto ppVTable = *reinterpret_cast<uintptr_t**>(pPage);

    // The base open skips this test and faults on a page the engine has not brought up yet.
    if (*(pBytes + nPageReady) == 0 || *reinterpret_cast<void**>(pBytes + nPageDocument) == nullptr)
        return;

    // The plan is a function of how many lines there are and what each holds, so that settles first.
    PrepareLayout(pPage);

    // Every open: the window it measures is one of the things this page changes.
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

    // Before the base open the engine hands out a preset template reading ultrahigh for all three,
    // so ask again on the way out and rebuild if the answer moved. Once per session.
    if (BeyondUltraAvailability() != nBuiltAvailability)
    {
        ShowPage(pPage, nCurrentPage, false);
    }

    // After the base open: it puts the selection on the first row, undoing anything settled before.
    SettleSelection(SafeRead<void*>(pPage, nPageList), 0, 1);
    bRowsStale = false;
}

// Replaces the page heading; the stock title is inline or on the heap depending on its length.
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

// The stock constructor with the MAGMA page name swapped out, so the clone binds to a document of
// its own. The name arrives as a push imm32, repointed for the duration of the call.
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

// One immediate out of BuildEntries names both the class to clone and its creator. The class record
// cannot be read yet: the creator fills it in lazily and it is zero until then.
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

// A class id is its record's last slot; step back until the depth field agrees with the distance.
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

    // The clone's record plus one link. Only the registry reads the last slot, so anything unique
    // does; our name string's address can neither collide nor be the 0xFFFFFFFF that means no page.
    JackalFixClassRecord[0] = reinterpret_cast<uint32_t>(szJackalFixPageClass);
    JackalFixClassRecord[1] = pRecord[1] + 1;
    for (uint32_t i = 0; i < pRecord[1]; i++)
        JackalFixClassRecord[2 + i] = pRecord[2 + i];
    JackalFixClassRecord[pRecord[1] + 2] = reinterpret_cast<uint32_t>(szJackalFixPageClass);

    pJackalFixClassId = &JackalFixClassRecord[JackalFixClassRecord[1] + 1];
    return true;
}

// Asked every time: the registry is rebuilt with the menu and a previous manager's page is gone.
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

        // Field offset and base entry point out of the stock open; a mismatch aborts.
        auto pOpen = reinterpret_cast<uint8_t*>(JackalFixVTable[1 + nOpenSlot]);
        if (pOpen[nOpenBuildRows] != 0xE8 || pOpen[nOpenBaseTailJump] != 0xE9)
            return false;

        nPageResetField = *reinterpret_cast<int32_t*>(pOpen + nOpenResetFieldDisp);
        BasePageOpen = reinterpret_cast<PageMethod_t>(RelativeTarget(pOpen + nOpenBaseTailJump, 1));

        // Prompt bookkeeping out of the base open; a mismatch costs only APPLY's light on a rebuild.
        auto pBase = reinterpret_cast<uint8_t*>(BasePageOpen);
        if (pBase[nBaseOpenDirtyOpcode] == 0xC6 && pBase[nBaseOpenDirtyOpcode + 1] == 0x87
            && pBase[nBaseOpenPromptSlot] == 0xE8 && pBase[nBaseOpenSetEnabled] == 0xE8)
        {
            nPageChanged     = *reinterpret_cast<int32_t*>(pBase + nBaseOpenDirtyDisp);
            SetPromptEnabled = reinterpret_cast<SetPromptEnabled_t>(CallTarget(pBase + nBaseOpenSetEnabled));
        }

        // Never run, but its prologue is where the action handler wiring is spelled out.
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

        // The three slots unsafe on a clone.
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

static SafetyHookInline BuildEntriesHook{};
static SafetyHookInline PageCreatorHook{};

// Same pass as the engine's own: a page registered later gets no document and no ready flag, and
// opening it walks null.
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

    // The row goes in either way; a missing page adds it greyed rather than skipping it.
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

            // CFCXOptionPage::BuildEntries, anchored on the 7Ch delegate allocation.
            auto buildPattern = dunia_pattern("83 EC 40 53 55 56 57 33 DB 53 6A 7C 8B F1 E8 ? ? ? ? 8B F8 83 C4 08 3B FB 74 23");
            if (buildPattern.empty())
                return;

            // CListMenuPage::AddValueRow, anchored on the 5Ch control allocation.
            auto rowPattern = dunia_pattern("51 8B 44 24 20 8B 54 24 1C 53 55 56 57 8B 7C 24 18 50 52 57 89 4C 24 1C E8 ? ? ? ? 33 DB 8B E8 53 6A 5C");
            if (rowPattern.empty())
                return;

            // AddMultiValueRow. Anchored on the 60h allocation and the vtable store after it.
            auto multiPattern = dunia_pattern("8B 44 24 20 53 55 8B 6C 24 0C 56 57 8B F9 8B 4C 24 2C 50 51 55 8B CF E8 ? ? ? ? 33 DB 53 6A 60");
            if (multiPattern.empty())
                return;

            // The page creation template, instantiated once per page class.
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

            // No slot plan yet: it needs the line count, which needs the document.
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

            // The front page mark; without this only the mark is skipped, not the version line.
            RenderState = reinterpret_cast<RenderState_t>(ResolveEngineFunction(0x004EE7F0,
                "8B 0D ? ? ? ? 56 8B 35 ? ? ? ? E8 ? ? ? ? 8B 44 B0 0C 5E C3"));

            auto pReaderRead = ResolveEngineFunction(0x00AE7BF0,
                "56 8B F1 83 7E 10 00 57 75 0D 8B 4E 18 8B 01 8B 50 10 FF D2 89 46 10");

            if (pReaderRead != nullptr)
                ReaderReadHook = safetyhook::create_inline(pReaderRead, ReaderRead);

            BuildEntriesHook = safetyhook::create_inline(pBuildEntries, BuildEntries);

            // Picks up an outside edit on a page already open; our own writes are skipped.
            JackalFix::onIniFileChange() += []()
            {
                if (bApplyingOurselves || nSlots == 0)
                    return;

                // Our writes return through the watcher after the flag clears, so compare rather
                // than race the timing.
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

                // Watcher's thread: reading is safe, tearing the rows down mid-draw faults. Take
                // the values now, redraw on the engine's thread at the next key or open.
                ResetPendingValues();
                bRowsStale = true;

            };

            // Only the creator is resolved here; our own class record waits for it to run.
            if (!ResolveCreator())
                return;

            PageCreatorHook = safetyhook::create_inline(pPageCreator, CreateGamePage);
        };
    }
} JackalFixMenu;
