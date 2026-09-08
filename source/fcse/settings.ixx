module;

#include <common.hxx>
#include <fcse_api.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

export module settings;

import common;
import logging;

// The preferences the ASI build reads from FC2JackalFix.ini, declared to FCSE instead. FCSE owns
// every value, keeps them in bin\fcse.ini under [FC2JackalFix] and gives each one a row on its Mod
// Configuration Menu, so this module has no file of its own to read or watch.
export enum Pref
{
    PREF_SKIPINTRO,
    PREF_SKIPTITLESCREEN,
    PREF_SKIPTUTORIALS,
    PREF_FIELDOFVIEW,
    PREF_VIEWMODELFIELDOFVIEW,
    PREF_IRONSIGHTFIELDOFVIEW,
    PREF_VEHICLEFIELDOFVIEW,
    PREF_MOUSESPEEDCAP,
    PREF_MOUSELOOKSENSITIVITY,
    PREF_CONTROLLERLOOKSENSITIVITY,
    PREF_SPRINTTURNMODIFIER,
    PREF_AIMTOGGLE,
    PREF_SPRINTTOGGLE,
    PREF_AIMTOGGLECONTROLLER,
    PREF_AIMASSIST,
    PREF_CUSTOMBINDS,
    PREF_VIBRATION,
    PREF_CONTROLLERPROMPTS,
    PREF_HIGHPRECISIONTIMER,
    PREF_SKIPSYSTEMDETECTION,
    PREF_LARGEADDRESSAWARE,
    PREF_MAXFRAMERATE,
    PREF_ANISOTROPICFILTERING,
    PREF_NORIMLIGHTING,
    PREF_NOSPRINTAIMBLUR,
    PREF_SATURATION,
    PREF_BEYONDULTRAGEOMETRY,
    PREF_BEYONDULTRASHADOWS,
    PREF_SHADOWRESOLUTION,
    PREF_BEYONDULTRATERRAIN,
    PREF_DISPLAYMODE,
    PREF_INTERNALRESOLUTIONX,
    PREF_INTERNALRESOLUTIONY,
    PREF_SCALINGFILTER,
    PREF_FPSCOUNTER,
    PREF_CPUAFFINITY,
    PREF_UTILISATION,
    PREF_LIMITEDSAVING,
    PREF_CONSOLEAUTOSAVES,
    PREF_NOBLINKINGITEMS,
    PREF_NOHITINDICATOR,
    PREF_HUDFADEOUT,
    PREF_PREDECESSORTAPES,
    PREF_MACHETES,
    PREF_DEBUGINVINCIBILITY,
    PREF_DEBUGINFINITEAMMO,
    PREF_DEBUGUNLOCKALLWEAPONS,
    PREF_DEBUGDIAMONDS,
    PREF_DEBUGNOCLIP,
    PREF_DEBUGNOCLIPKEY,
    PREF_DEBUGFREECAM,
    PREF_DEBUGFREECAMKEY,

    COUNT,
};

enum PrefKind
{
    KIND_NONE,       // FCSE already covers it, so it is never offered
    KIND_BOOL,       // int, 0 or 1
    KIND_BOOL_FLOAT, // float, 0.0 or 1.0
    KIND_ENUM,       // int taken from Values
    KIND_STEPS,      // float, offered as a ladder of formatted steps
    KIND_SLIDER,     // int between nMinimum and nMaximum
    KIND_TEXT,       // string
    KIND_TEXT_INT,   // int typed in, for the ranges too wide to pick from a list
};

// One preference as FCSE is told about it. The row label doubles as the ini key, so it names its
// section wherever two sections would otherwise collide.
struct PrefSpec
{
    PrefKind    nKind;
    const char* pName;
    bool        bHidden;

    int         nDefault;     // KIND_BOOL, KIND_ENUM, KIND_SLIDER
    float       fDefault;     // KIND_BOOL_FLOAT, KIND_STEPS
    const char* pDefaultText; // KIND_TEXT, KIND_TEXT_INT

    // KIND_STEPS ladder ends and stride, in the setting's own units times fScale. KIND_SLIDER
    // bounds, in its own units.
    int         nMinimum;
    int         nMaximum;
    int         nStep;
    float       fScale;
    const char* pFormat;
    bool        bOffFirst;    // KIND_STEPS: lead the ladder with an Off worth zero

    const int*         pValues; // KIND_ENUM
    const char* const* pLabels;
    uint32_t           nValueCount;
};

static constexpr PrefSpec None()
{
    return { KIND_NONE, nullptr, false, 0, 0.0f, nullptr, 0, 0, 1, 1.0f, nullptr, false, nullptr, nullptr, 0 };
}

static constexpr PrefSpec Boolean(const char* pName, int nDefault, bool bHidden = false)
{
    return { KIND_BOOL, pName, bHidden, nDefault, 0.0f, nullptr, 0, 1, 1, 1.0f, nullptr, false, nullptr, nullptr, 0 };
}

// A boolean the game stores as a float, because what it drives is a multiplier.
static constexpr PrefSpec BooleanFloat(const char* pName, float fDefault)
{
    return { KIND_BOOL_FLOAT, pName, false, 0, fDefault, nullptr, 0, 1, 1, 1.0f, nullptr, false, nullptr, nullptr, 0 };
}

template<size_t N>
static constexpr PrefSpec Enumeration(const char* pName, int nDefault,
                                      const int (&Values)[N], const char* const (&Labels)[N])
{
    return { KIND_ENUM, pName, false, nDefault, 0.0f, nullptr, 0, 0, 1, 1.0f, nullptr, false, Values, Labels, N };
}

static constexpr PrefSpec Steps(const char* pName, float fDefault, int nMinimum, int nMaximum,
                                int nStep, float fScale, const char* pFormat, bool bOffFirst = false)
{
    return { KIND_STEPS, pName, false, 0, fDefault, nullptr, nMinimum, nMaximum, nStep, fScale,
             pFormat, bOffFirst, nullptr, nullptr, 0 };
}

static constexpr PrefSpec Slider(const char* pName, int nDefault, int nMinimum, int nMaximum,
                                 bool bHidden = false)
{
    return { KIND_SLIDER, pName, bHidden, nDefault, 0.0f, nullptr, nMinimum, nMaximum, 1, 1.0f,
             nullptr, false, nullptr, nullptr, 0 };
}

static constexpr PrefSpec Text(const char* pName, const char* pDefault, bool bHidden = false)
{
    return { KIND_TEXT, pName, bHidden, 0, 0.0f, pDefault, 0, 0, 1, 1.0f, nullptr, false, nullptr, nullptr, 0 };
}

static constexpr PrefSpec TextInt(const char* pName, const char* pDefault, bool bHidden = false)
{
    return { KIND_TEXT_INT, pName, bHidden, 0, 0.0f, pDefault, 0, 0, 1, 1.0f, nullptr, false,
             nullptr, nullptr, 0 };
}

// The choice lists, as the ASI build's own menu offers them.

static const int         DisplayModeValues[]{ 1, 2, 3 };
static const char* const DisplayModeLabels[]{ "Fullscreen", "Borderless", "Windowed" };

static const int         ScalingFilterValues[]{ 0, 1 };
static const char* const ScalingFilterLabels[]{ "Point", "Bilinear" };

static const int         MaxFrameRateValues[]{ 0, 1, 30, 60, 72, 75, 90, 120, 144, 165, 180, 240 };
static const char* const MaxFrameRateLabels[]{ "Unlocked", "Screen Refresh", "30Hz", "60Hz", "72Hz",
                                               "75Hz", "90Hz", "120Hz", "144Hz", "165Hz", "180Hz", "240Hz" };

static const int         AnisotropyValues[]{ 0, 2, 4, 8, 16 };
static const char* const AnisotropyLabels[]{ "Game default", "2x", "4x", "8x", "16x" };

static const int         BeyondUltraValues[]{ 0, 1, 2, 3 };
static const char* const BeyondUltraLabels[]{ "Default", "2x", "4x", "Max draw distance" };

static const int         BeyondUltraGeometryValues[]{ 0, 1, 2, 3, 4 };
static const char* const BeyondUltraGeometryLabels[]{ "Default", "2x", "4x", "6x", "Max draw distance" };

static const int         BeyondUltraShadowValues[]{ 0, 1 };
static const char* const BeyondUltraShadowLabels[]{ "Default", "Max draw distance" };

static const int         SkipTutorialsValues[]{ 0, 1, 2 };
static const char* const SkipTutorialsLabels[]{ "Off", "Pop-ups", "Pop-ups and hints" };

static const int         HudFadeOutValues[]{ -2, 1, 2, 0, 5, 8, 10, 15, 20, 30, 60, -1 };
static const char* const HudFadeOutLabels[]{ "HUD Always Hidden", "1s", "2s", "3s (Default)", "5s",
                                             "8s", "10s", "15s", "20s", "30s", "60s", "HUD Always Visible" };

// Defaults are the shipped FC2JackalFix.ini's rather than ReadIniSettings' fallbacks: the shipped
// file is what a player installing the ASI build actually runs with.
static const PrefSpec PrefSpecs[Pref::COUNT]
{
    Boolean("Skip Intro", 1),
    Boolean("Skip Title Screen", 1),
    Enumeration("Skip Tutorials", 0, SkipTutorialsValues, SkipTutorialsLabels),
    Steps("Field of View", 91.31f, 4500, 14000, 500, 100.0f, "%g"),
    Steps("Viewmodel FOV", 75.0f, 4500, 14000, 500, 100.0f, "%g"),
    Steps("Ironsight FOV", 59.85f, 2000, 14000, 500, 100.0f, "%g", true),
    Steps("Vehicle FOV", 91.31f, 4500, 14000, 500, 100.0f, "%g", true),
    Boolean("Remove Mouse Speed Cap", 1),
    Steps("Mouse Look Sensitivity", 1.0f, 10, 500, 5, 100.0f, "%.2f"),
    Steps("Controller Look Sensitivity", 1.0f, 5, 200, 5, 100.0f, "%.2f"),
    BooleanFloat("Sprint Turn Modifier", 1.0f),
    Boolean("Aim Toggle", 1),
    Boolean("Sprint Toggle", 1),
    Boolean("Controller Aim Toggle", 1),
    Boolean("Aim Assist", 1),
    Boolean("Custom Binds", 1),
    Boolean("Vibration", 1),
    Boolean("Controller Prompts", 1),
    Boolean("High Precision Timer", 1),
    Boolean("Skip System Detection", 0),
    None(), // PREF_LARGEADDRESSAWARE: FCSE.exe is linked large address aware already
    Enumeration("Max Frame Rate", 1, MaxFrameRateValues, MaxFrameRateLabels),
    Enumeration("Anisotropic Filtering", 16, AnisotropyValues, AnisotropyLabels),
    Boolean("No Rim Lighting", 0),
    Boolean("No Sprint And Aim Blur", 0),
    Steps("Saturation", 0.5f, 0, 100, 5, 100.0f, "%.2f"),
    Enumeration("Beyond Ultra Geometry", 0, BeyondUltraGeometryValues, BeyondUltraGeometryLabels),
    Enumeration("Beyond Ultra Shadows", 0, BeyondUltraShadowValues, BeyondUltraShadowLabels),
    TextInt("Shadow Resolution", "2048", true),
    Enumeration("Beyond Ultra Terrain", 0, BeyondUltraValues, BeyondUltraLabels),
    Enumeration("Display Mode", 2, DisplayModeValues, DisplayModeLabels),
    TextInt("Internal Resolution X", "0"),
    TextInt("Internal Resolution Y", "0"),
    Enumeration("Scaling Filter", 1, ScalingFilterValues, ScalingFilterLabels),
    Boolean("FPS Counter", 0),
    Slider("CPU Affinity", 0, 0, 64),
    Boolean("Improved Utilisation", 1),
    Boolean("Limited Saving", 0),
    Boolean("Console Autosaves", 1),
    Boolean("No Blinking Items", 0),
    Boolean("No Hit Indicator", 0),
    Enumeration("HUD Fade Out", 0, HudFadeOutValues, HudFadeOutLabels),
    Boolean("Predecessor Tapes", 1),
    Boolean("Machetes", 1),
    Boolean("Invincibility", 0, true),
    Boolean("Infinite Ammo", 0, true),
    Boolean("Unlock All Weapons", 0, true),
    Slider("Diamonds", 0, 0, 999, true),
    Boolean("Noclip", 0, true),
    Text("Noclip Key", "F1", true),
    Boolean("Freecam", 0, true),
    Text("Freecam Key", "F2", true),
};

// A KIND_STEPS ladder, built once. FCSE serialises a choice by its label, so the value a label
// stands for has to stay reachable for the session; the labels themselves it copies.
struct StepLadder
{
    std::vector<float>       Values;
    std::vector<std::string> Text;
    std::vector<const char*> Labels;
    uint32_t                 nDefault = 0;
};

static StepLadder Ladders[Pref::COUNT];

static int32_t ParseInt(const char* pText, int32_t nFallback = 0)
{
    return pText != nullptr ? static_cast<int32_t>(std::strtol(pText, nullptr, 10)) : nFallback;
}

static std::string FormatStep(const PrefSpec& spec, float fValue)
{
    char szText[64]{};
    std::snprintf(szText, sizeof(szText), spec.pFormat, static_cast<double>(fValue));
    return szText;
}

// The ladder for one setting: the steps its range describes, plus the default wherever it falls
// between two of them. Compared by label rather than by value, because the label is the identity
// FCSE stores and two of them must never collide.
static void BuildLadder(Pref nPref)
{
    const auto& spec = PrefSpecs[nPref];
    auto& ladder = Ladders[nPref];

    auto Add = [&](float fValue, std::string szText)
    {
        ladder.Values.push_back(fValue);
        ladder.Text.push_back(std::move(szText));
    };

    if (spec.bOffFirst)
        Add(0.0f, "Off");

    // A default of zero on a ladder that leads with Off is that Off entry, already in at index 0,
    // which is where nDefault starts.
    const auto szDefault = FormatStep(spec, spec.fDefault);
    auto bDefaultPlaced = spec.bOffFirst && spec.fDefault <= 0.0f;

    for (auto nStep = spec.nMinimum; nStep <= spec.nMaximum; nStep += spec.nStep)
    {
        const auto fValue = static_cast<float>(nStep) / spec.fScale;
        auto szText = FormatStep(spec, fValue);

        if (!bDefaultPlaced && spec.fDefault < fValue)
        {
            Add(spec.fDefault, szDefault);
            bDefaultPlaced = true;
        }

        if (szText == szDefault)
            bDefaultPlaced = true;

        Add(fValue, std::move(szText));
    }

    if (!bDefaultPlaced)
        Add(spec.fDefault, szDefault);

    // Only now the strings have stopped moving, or every pointer would dangle.
    for (size_t i = 0; i < ladder.Text.size(); i++)
    {
        ladder.Labels.push_back(ladder.Text[i].c_str());

        if (ladder.Text[i] == szDefault)
            ladder.nDefault = static_cast<uint32_t>(i);
    }
}

using PrefValue = std::variant<int32_t, float, std::string>;

static std::array<PrefValue, static_cast<size_t>(Pref::COUNT)> mPrefs;

// Raised once every setting has been registered, so the run of callbacks that registration itself
// makes does not broadcast a change per setting before the first one is even applied.
static bool bRegistered = false;

// Both axes or neither, then each to what a D3D9 render target can describe. The pair is one
// decision, so it is settled whenever either half moves.
static void SettleInternalResolution()
{
    auto nX = std::get<int32_t>(mPrefs[PREF_INTERNALRESOLUTIONX]);
    auto nY = std::get<int32_t>(mPrefs[PREF_INTERNALRESOLUTIONY]);

    if (nX < 1 || nY < 1)
    {
        nX = 0;
        nY = 0;
    }
    else
    {
        nX = std::clamp(nX, 320, 16384);
        nY = std::clamp(nY, 240, 16384);
    }

    mPrefs[PREF_INTERNALRESOLUTIONX] = nX;
    mPrefs[PREF_INTERNALRESOLUTIONY] = nY;
}

static void StorePref(Pref nPref, const FCSE_SettingValue& value)
{
    const auto& spec = PrefSpecs[nPref];

    switch (spec.nKind)
    {
    case KIND_BOOL:
        mPrefs[nPref] = value.asCheckbox ? 1 : 0;
        break;

    case KIND_BOOL_FLOAT:
        mPrefs[nPref] = value.asCheckbox ? 1.0f : 0.0f;
        break;

    case KIND_ENUM:
        if (value.asChoice < spec.nValueCount)
            mPrefs[nPref] = spec.pValues[value.asChoice];
        break;

    case KIND_STEPS:
        if (value.asChoice < Ladders[nPref].Values.size())
            mPrefs[nPref] = Ladders[nPref].Values[value.asChoice];
        break;

    case KIND_SLIDER:
        mPrefs[nPref] = value.asSlider;
        break;

    case KIND_TEXT:
        mPrefs[nPref] = std::string(value.asText != nullptr ? value.asText : "");
        break;

    case KIND_TEXT_INT:
        mPrefs[nPref] = ParseInt(value.asText, ParseInt(spec.pDefaultText));
        break;

    default:
        break;
    }

    if (nPref == PREF_INTERNALRESOLUTIONX || nPref == PREF_INTERNALRESOLUTIONY)
        SettleInternalResolution();
}

// FCSE calls this once per setting as it registers, carrying whatever bin\fcse.ini held, and again
// after every change made in game.
static void __cdecl OnPrefChanged(const FCSE_SettingValue* pValue, void* pUserdata)
{
    if (pValue == nullptr)
        return;

    StorePref(static_cast<Pref>(reinterpret_cast<uintptr_t>(pUserdata)), *pValue);

    // What every ApplyAndWatch hangs off. The ASI build reaches it from its ini watcher instead.
    if (bRegistered)
        JackalFix::onIniFileChange().executeAll();
}

static void SeedDefault(Pref nPref)
{
    const auto& spec = PrefSpecs[nPref];

    switch (spec.nKind)
    {
    case KIND_BOOL_FLOAT:
    case KIND_STEPS:
        mPrefs[nPref] = spec.fDefault;
        break;

    case KIND_TEXT:
        mPrefs[nPref] = std::string(spec.pDefaultText != nullptr ? spec.pDefaultText : "");
        break;

    case KIND_TEXT_INT:
        mPrefs[nPref] = ParseInt(spec.pDefaultText);
        break;

    default:
        mPrefs[nPref] = spec.nDefault;
        break;
    }
}

static FCSE_Setting DescribeSetting(Pref nPref)
{
    const auto& spec = PrefSpecs[nPref];

    FCSE_Setting setting{};
    setting.name = spec.pName;
    setting.onChanged = &OnPrefChanged;
    setting.userdata = reinterpret_cast<void*>(static_cast<uintptr_t>(nPref));
    setting.flags = spec.bHidden ? static_cast<uint32_t>(FCSE_SettingFlag_Hidden) : 0u;

    switch (spec.nKind)
    {
    case KIND_BOOL:
        setting.defaultValue.type = FCSE_SettingType_Checkbox;
        setting.defaultValue.asNumber = spec.nDefault != 0 ? 1 : 0;
        break;

    case KIND_BOOL_FLOAT:
        setting.defaultValue.type = FCSE_SettingType_Checkbox;
        setting.defaultValue.asNumber = spec.fDefault > 0.0f ? 1 : 0;
        break;

    case KIND_ENUM:
    {
        uint32_t nDefault = 0;
        for (uint32_t i = 0; i < spec.nValueCount; i++)
        {
            if (spec.pValues[i] == spec.nDefault)
                nDefault = i;
        }

        setting.defaultValue.type = FCSE_SettingType_Choice;
        setting.defaultValue.asNumber = static_cast<int32_t>(nDefault);
        setting.choices = spec.pLabels;
        setting.choiceCount = spec.nValueCount;
        break;
    }

    case KIND_STEPS:
        setting.defaultValue.type = FCSE_SettingType_Choice;
        setting.defaultValue.asNumber = static_cast<int32_t>(Ladders[nPref].nDefault);
        setting.choices = Ladders[nPref].Labels.data();
        setting.choiceCount = static_cast<uint32_t>(Ladders[nPref].Labels.size());
        break;

    case KIND_SLIDER:
        setting.defaultValue.type = FCSE_SettingType_Slider;
        setting.defaultValue.asNumber = spec.nDefault;
        setting.minValue = spec.nMinimum;
        setting.maxValue = spec.nMaximum;
        break;

    case KIND_TEXT:
    case KIND_TEXT_INT:
        setting.defaultValue.type = FCSE_SettingType_Text;
        setting.defaultText = spec.pDefaultText;
        break;

    default:
        break;
    }

    return setting;
}

// Declares every preference to FCSE, which answers with the value bin\fcse.ini holds for each. Call
// it once, from FCSE_Load, before anything subscribes to a setting.
export void RegisterJackalFixSettings()
{
    std::vector<FCSE_Setting> Settings;
    Settings.reserve(static_cast<size_t>(Pref::COUNT));

    for (size_t i = 0; i < static_cast<size_t>(Pref::COUNT); i++)
    {
        const auto nPref = static_cast<Pref>(i);
        SeedDefault(nPref);

        if (PrefSpecs[i].nKind == KIND_NONE)
            continue;

        if (PrefSpecs[i].nKind == KIND_STEPS)
            BuildLadder(nPref);

        Settings.push_back(DescribeSetting(nPref));
    }

    // Null would mean the modules and the entry point ended up with separate copies of the API
    // pointer, in which case every hook is silently a no-op too. Worth a line rather than a
    // mod that loads and does nothing.
    const FCSE_PluginAPI* pApi = FCSE::ApiPointer();
    if (pApi == nullptr)
    {
        LogWarn("Settings: no FCSE API, so nothing was registered and no setting has a value");
        return;
    }

    pApi->RegisterSettings("FC2JackalFix", Settings.data(), Settings.size());
    LogInfo("Settings: registered {} preferences with FCSE", Settings.size());

    bRegistered = true;
}

export class CSettings
{
public:
    int32_t GetInt(Pref name) { return std::get<int32_t>(mPrefs[name]); }
    float GetFloat(Pref name) { return std::get<float>(mPrefs[name]); }
    std::string GetString(Pref name) { return std::get<std::string>(mPrefs[name]); }
} JackalFixSettings;

// Reads a setting into a variable now and on every change; T covers atomics too.
export template<class T>
void BindBool(T& target, Pref pref)
{
    ApplyAndWatch([&target, pref]() { target = JackalFixSettings.GetInt(pref) != 0; });
}

export template<class T>
void BindInt(T& target, Pref pref)
{
    ApplyAndWatch([&target, pref]() { target = JackalFixSettings.GetInt(pref); });
}

export template<class T>
void BindFloat(T& target, Pref pref)
{
    ApplyAndWatch([&target, pref]() { target = JackalFixSettings.GetFloat(pref); });
}

// Byte patch written while the setting is on, restored while it is off.
export void BindPatch(raw_mem& patch, Pref pref)
{
    ApplyAndWatch([&patch, pref]() { patch.Set(JackalFixSettings.GetInt(pref) != 0); });
}
