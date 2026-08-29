module;

#include <common.hxx>
#include <FileWatch.hpp>
#include <variant>
#include <atomic>

export module settings;

import common;

// Raised while the menu writes the file: mINI truncates and rewrites, and a re-read landing on the
// truncation would read an empty ini and default everything. The finished write fires again.
export inline std::atomic<bool> bJackalFixWritingIni = false;

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

export class CSettings
{
private:
    using PrefValue = std::variant<int32_t, float, std::string>;

    static inline std::array<PrefValue, static_cast<size_t>(Pref::COUNT)> mPrefs;

    // Settings moved for this run only, file untouched. A hold keeps the chosen value and the file
    // value at the time; a re-read differing from the latter means a hand edit and drops the hold.
    static inline std::array<bool, static_cast<size_t>(Pref::COUNT)> mHeld{};
    static inline std::array<PrefValue, static_cast<size_t>(Pref::COUNT)> mHeldValue;
    static inline std::array<PrefValue, static_cast<size_t>(Pref::COUNT)> mHeldFile;

    // What the file says, never written over by a hold.
    static inline std::array<PrefValue, static_cast<size_t>(Pref::COUNT)> mFile;

    // Which keys the ini actually carries: a default reads the same as a written 0, and the debug
    // rows must tell those apart. Only keys asked about are recorded.
    static inline std::array<bool, static_cast<size_t>(Pref::COUNT)> mInFile{};

    // No exists query, so read as a string against a default no ini would hold.
    static bool KeyInFile(CIniReader& reader, const char* szSection, const char* szKey)
    {
        static constexpr auto szMissing = "\x1";
        return reader.ReadString(szSection, szKey, szMissing) != szMissing;
    }

public:
    static inline void ReadIniSettings()
    {
        CIniReader iniReader("");
        mPrefs[PREF_SKIPINTRO] = std::clamp(iniReader.ReadInteger("General", "SkipIntro", 1), 0, 1);
        mPrefs[PREF_SKIPTITLESCREEN] = std::clamp(iniReader.ReadInteger("General", "SkipTitleScreen", 1), 0, 1);

        mPrefs[PREF_SKIPTUTORIALS] = std::clamp(iniReader.ReadInteger("General", "SkipTutorials", 0), 0, 2);

        mPrefs[PREF_CPUAFFINITY] = std::clamp(iniReader.ReadInteger("General", "CpuAffinity", 0), 0, 64);
        mPrefs[PREF_UTILISATION] = std::clamp(iniReader.ReadInteger("Graphics", "ImprovedUtilisation", 1), 0, 1);
        mPrefs[PREF_HIGHPRECISIONTIMER] = std::clamp(iniReader.ReadInteger("General", "HighPrecisionTimer", 1), 0, 1);
        mPrefs[PREF_SKIPSYSTEMDETECTION] = std::clamp(iniReader.ReadInteger("General", "SkipSystemDetection", 1), 0, 1);
        mPrefs[PREF_LARGEADDRESSAWARE] = std::clamp(iniReader.ReadInteger("General", "LargeAddressAware", 0), 0, 1);

        // 1 fullscreen, 2 borderless, 3 windowed.
        mPrefs[PREF_DISPLAYMODE] = std::clamp(iniReader.ReadInteger("Display", "DisplayMode", 2), 1, 3);

        // Both axes or neither; ceiling is the largest square a D3D9 render target can describe.
        auto nInternalResolutionX = iniReader.ReadInteger("Display", "InternalResolutionX", 0);
        auto nInternalResolutionY = iniReader.ReadInteger("Display", "InternalResolutionY", 0);
        if (nInternalResolutionX < 1 || nInternalResolutionY < 1)
        {
            nInternalResolutionX = 0;
            nInternalResolutionY = 0;
        }
        else
        {
            nInternalResolutionX = std::clamp(nInternalResolutionX, 320, 16384);
            nInternalResolutionY = std::clamp(nInternalResolutionY, 240, 16384);
        }
        mPrefs[PREF_INTERNALRESOLUTIONX] = nInternalResolutionX;
        mPrefs[PREF_INTERNALRESOLUTIONY] = nInternalResolutionY;

        mPrefs[PREF_SCALINGFILTER] = std::clamp(iniReader.ReadInteger("Display", "ScalingFilter", 1), 0, 1);
        mPrefs[PREF_FPSCOUNTER] = std::clamp(iniReader.ReadInteger("Display", "FpsCounter", 0), 0, 1);

        mPrefs[PREF_MAXFRAMERATE] = std::clamp(iniReader.ReadInteger("Display", "MaxFrameRate", 1), 0, 9999);

        mPrefs[PREF_ANISOTROPICFILTERING] = std::clamp(iniReader.ReadInteger("Graphics", "AnisotropicFiltering", 16), 0, 16);
        mPrefs[PREF_NORIMLIGHTING] = std::clamp(iniReader.ReadInteger("Graphics", "NoRimLighting", 0), 0, 1);
        mPrefs[PREF_NOSPRINTAIMBLUR] = std::clamp(iniReader.ReadInteger("Graphics", "NoSprintAimBlur", 0), 0, 1);

        mPrefs[PREF_SATURATION] = std::clamp(iniReader.ReadFloat("Graphics", "Saturation", 0.5f), 0.0f, 4.0f);

        mPrefs[PREF_BEYONDULTRAGEOMETRY] = std::clamp(iniReader.ReadInteger("BeyondUltra", "BeyondUltraGeometry", 4), 0, 4);
        mPrefs[PREF_BEYONDULTRASHADOWS] = std::clamp(iniReader.ReadInteger("BeyondUltra", "BeyondUltraShadows", 1), 0, 1);

        // Unclamped on purpose: the point is to go past the presets, and the device decides.
        mPrefs[PREF_SHADOWRESOLUTION] = iniReader.ReadInteger("BeyondUltra", "ShadowResolution", 2048);

        mPrefs[PREF_BEYONDULTRATERRAIN] = std::clamp(iniReader.ReadInteger("BeyondUltra", "BeyondUltraTerrain", 3), 0, 3);

        mPrefs[PREF_MOUSESPEEDCAP] = std::clamp(iniReader.ReadInteger("Gameplay", "RemoveMouseSpeedCap", 1), 0, 1);

        auto nAimToggle = std::clamp(iniReader.ReadInteger("Gameplay", "AimToggle", 0), 0, 1);
        auto nSprintToggle = std::clamp(iniReader.ReadInteger("Gameplay", "SprintToggle", 0), 0, 1);
        mPrefs[PREF_AIMTOGGLE] = nAimToggle;
        mPrefs[PREF_SPRINTTOGGLE] = nSprintToggle;

        mPrefs[PREF_LIMITEDSAVING] = std::clamp(iniReader.ReadInteger("Gameplay", "LimitedSaving", 0), 0, 1);
        mPrefs[PREF_CONSOLEAUTOSAVES] = std::clamp(iniReader.ReadInteger("Gameplay", "ConsoleAutosaves", 1), 0, 1);
        mPrefs[PREF_NOBLINKINGITEMS] = std::clamp(iniReader.ReadInteger("Gameplay", "NoBlinkingItems", 0), 0, 1);
        mPrefs[PREF_NOHITINDICATOR] = std::clamp(iniReader.ReadInteger("Gameplay", "NoHitIndicator", 0), 0, 1);

        auto fMouseLookSensitivity = iniReader.ReadFloat("Gameplay", "MouseLookSensitivity", 1.0f);
        mPrefs[PREF_MOUSELOOKSENSITIVITY] = fMouseLookSensitivity <= 0.0f ? 1.0f : std::clamp(fMouseLookSensitivity, 0.01f, 5.0f);

        auto fSprintTurnModifier = iniReader.ReadFloat("Gameplay", "SprintTurnModifier", 0.0f);
        mPrefs[PREF_SPRINTTURNMODIFIER] = fSprintTurnModifier <= 0.0f ? 0.0f : std::clamp(fSprintTurnModifier, 0.01f, 2.0f);

        mPrefs[PREF_FIELDOFVIEW] = std::clamp(iniReader.ReadFloat("FieldOfView", "FieldOfView", 75.0f), 45.0f, 140.0f);
        mPrefs[PREF_VIEWMODELFIELDOFVIEW] = std::clamp(iniReader.ReadFloat("FieldOfView", "ViewmodelFieldOfView", 75.0f), 45.0f, 140.0f);

        auto fIronsightFieldOfView = iniReader.ReadFloat("FieldOfView", "IronsightFieldOfView", 0.0f);
        mPrefs[PREF_IRONSIGHTFIELDOFVIEW] = fIronsightFieldOfView <= 0.0f ? 0.0f : std::clamp(fIronsightFieldOfView, 20.0f, 140.0f);

        auto fVehicleFieldOfView = iniReader.ReadFloat("FieldOfView", "VehicleFieldOfView", 0.0f);
        mPrefs[PREF_VEHICLEFIELDOFVIEW] = fVehicleFieldOfView <= 0.0f ? 0.0f : std::clamp(fVehicleFieldOfView, 45.0f, 140.0f);

        auto fControllerLookSensitivity = iniReader.ReadFloat("Controller", "ControllerLookSensitivity", 1.0f);
        mPrefs[PREF_CONTROLLERLOOKSENSITIVITY] = fControllerLookSensitivity <= 0.0f ? 1.0f : std::clamp(fControllerLookSensitivity, 0.01f, 5.0f);

        mPrefs[PREF_AIMASSIST] = std::clamp(iniReader.ReadInteger("Controller", "AimAssist", 1), 0, 1);
        mPrefs[PREF_VIBRATION] = std::clamp(iniReader.ReadInteger("Controller", "Vibration", 1), 0, 1);
        mPrefs[PREF_CONTROLLERPROMPTS] = std::clamp(iniReader.ReadInteger("Controller", "ControllerPrompts", 1), 0, 1);

        mPrefs[PREF_AIMTOGGLECONTROLLER] = std::clamp(iniReader.ReadInteger("Controller", "AimToggle", nAimToggle), 0, 1);

        mPrefs[PREF_PREDECESSORTAPES] = std::clamp(iniReader.ReadInteger("ContentUnlocks", "PredecessorTapesUnlock", 1), 0, 1);
        mPrefs[PREF_MACHETES] = std::clamp(iniReader.ReadInteger("ContentUnlocks", "MachetesUnlock", 1), 0, 1);

        mPrefs[PREF_DEBUGINVINCIBILITY] = std::clamp(iniReader.ReadInteger("Debug", "Invincibility", 0), 0, 1);
        mPrefs[PREF_DEBUGINFINITEAMMO] = std::clamp(iniReader.ReadInteger("Debug", "InfiniteAmmo", 0), 0, 1);
        mPrefs[PREF_DEBUGUNLOCKALLWEAPONS] = std::clamp(iniReader.ReadInteger("Debug", "UnlockAllWeapons", 0), 0, 1);

        mPrefs[PREF_DEBUGDIAMONDS] = std::clamp(iniReader.ReadInteger("Debug", "Diamonds", 0), 0, 999);

        mPrefs[PREF_DEBUGNOCLIP] = std::clamp(iniReader.ReadInteger("Debug", "Noclip", 0), 0, 1);
        mPrefs[PREF_DEBUGFREECAM] = std::clamp(iniReader.ReadInteger("Debug", "Freecam", 0), 0, 1);

        mPrefs[PREF_DEBUGNOCLIPKEY] = iniReader.ReadString("Debug", "NoclipKey", "F1");
        mPrefs[PREF_DEBUGFREECAMKEY] = iniReader.ReadString("Debug", "FreecamKey", "F2");

        for (const auto& [pref, szKey] : std::initializer_list<std::pair<Pref, const char*>>{
                { PREF_DEBUGINVINCIBILITY, "Invincibility" },
                { PREF_DEBUGINFINITEAMMO, "InfiniteAmmo" },
                { PREF_DEBUGUNLOCKALLWEAPONS, "UnlockAllWeapons" },
                { PREF_DEBUGDIAMONDS, "Diamonds" },
                { PREF_DEBUGNOCLIP, "Noclip" },
                { PREF_DEBUGFREECAM, "Freecam" } })
        {
            mInFile[pref] = KeyInFile(iniReader, "Debug", szKey);
        }

        // Before the holds go back on, so this is the file and nothing else.
        mFile = mPrefs;

        // Session choices back on top, where the file has not moved. Here, not in the caller, so
        // the watcher's re-reads go through it too.
        for (size_t i = 0; i < static_cast<size_t>(Pref::COUNT); i++)
        {
            if (!mHeld[i])
                continue;

            if (mPrefs[i] == mHeldFile[i])
                mPrefs[i] = mHeldValue[i];
            else
                mHeld[i] = false;
        }

        static std::once_flag flag;
        std::call_once(flag, [&]()
        {
            if (std::filesystem::exists(iniReader.GetIniPath()))
            {
                static filewatch::FileWatch<std::string> watch(iniReader.GetIniPath().string(), [](const std::string&, const filewatch::Event change_type)
                {
                    if (change_type == filewatch::Event::modified && !bJackalFixWritingIni)
                    {
                        ReadIniSettings();
                        JackalFix::onIniFileChange().executeAll();
                    }
                });
            }
        });
    }

public:
    int32_t GetInt(Pref name) { return std::get<int32_t>(mPrefs[name]); }
    float GetFloat(Pref name) { return std::get<float>(mPrefs[name]); }
    std::string GetString(Pref name) { return std::get<std::string>(mPrefs[name]); }
    void SetInt(Pref name, int32_t value) { mPrefs[name] = value; }
    void SetFloat(Pref name, float value) { mPrefs[name] = value; }
    void SetString(Pref name, std::string value) { mPrefs[name] = value; }

    // Whether the ini carries the key at all, rather than a default standing in.
    bool IsInFile(Pref name) { return mInFile[name]; }

    // What the ini says, whatever has been applied over it since.
    int32_t GetFileInt(Pref name) { return std::get<int32_t>(mFile[name]); }
    float GetFileFloat(Pref name) { return std::get<float>(mFile[name]); }

    // Set for this run without writing the file, kept across re-reads. The file value is noted on
    // the first hold only, or a second move would mistake the previous choice for the file's.
    void HoldFloat(Pref name, float value)
    {
        if (!mHeld[name])
            mHeldFile[name] = mPrefs[name];

        mHeldValue[name] = value;
        mHeld[name] = true;
        mPrefs[name] = value;
    }

    void HoldInt(Pref name, int32_t value)
    {
        if (!mHeld[name])
            mHeldFile[name] = mPrefs[name];

        mHeldValue[name] = value;
        mHeld[name] = true;
        mPrefs[name] = value;
    }
} JackalFixSettings;

// Reads a setting into a variable now and on every ini re-read; T covers atomics too.
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
