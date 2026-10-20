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
    PREF_CUSTOMBINDS,
    PREF_VIBRATION,
    PREF_CONTROLLERPROMPTS,
    PREF_HIGHPRECISIONTIMER,
    PREF_SKIPSYSTEMDETECTION,
    PREF_LARGEADDRESSAWARE,
    PREF_PAUSEONFOCUSLOSS,
    PREF_MAXFRAMERATE,
    PREF_ANISOTROPICFILTERING,
    PREF_NORIMLIGHTING,
    PREF_NOSPRINTAIMBLUR,
    PREF_SATURATION,
    PREF_VOLUMETRICFOG,
    PREF_VOLUMETRICCLOUDS,
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

    static int32_t ReadInt(CIniReader& reader, const char* szSection, const char* szKey, int32_t nDefault,
        int32_t nMin, int32_t nMax)
    {
        return std::clamp(reader.ReadInteger(szSection, szKey, nDefault), nMin, nMax);
    }

    // Zero or below gives the fallback, anything else is clamped into range.
    static float ReadOptionalFloat(CIniReader& reader, const char* szSection, const char* szKey, float fFallback,
        float fMin, float fMax)
    {
        auto fValue = reader.ReadFloat(szSection, szKey, 0.0f);
        return fValue <= 0.0f ? fFallback : std::clamp(fValue, fMin, fMax);
    }

    static void ReadGeneral(CIniReader& iniReader)
    {
        mPrefs[PREF_SKIPINTRO] = ReadInt(iniReader, "General", "SkipIntro", 1, 0, 1);
        mPrefs[PREF_SKIPTITLESCREEN] = ReadInt(iniReader, "General", "SkipTitleScreen", 1, 0, 1);
        mPrefs[PREF_SKIPTUTORIALS] = ReadInt(iniReader, "General", "SkipTutorials", 0, 0, 2);
        mPrefs[PREF_CPUAFFINITY] = ReadInt(iniReader, "General", "CpuAffinity", 0, 0, 64);
        mPrefs[PREF_HIGHPRECISIONTIMER] = ReadInt(iniReader, "General", "HighPrecisionTimer", 1, 0, 1);
        mPrefs[PREF_SKIPSYSTEMDETECTION] = ReadInt(iniReader, "General", "SkipSystemDetection", 1, 0, 1);
        mPrefs[PREF_LARGEADDRESSAWARE] = ReadInt(iniReader, "General", "LargeAddressAware", 0, 0, 1);
        mPrefs[PREF_PAUSEONFOCUSLOSS] = ReadInt(iniReader, "General", "PauseOnFocusLoss", 1, 0, 1);
    }

    static void ReadDisplay(CIniReader& iniReader)
    {
        // 1 fullscreen, 2 borderless, 3 windowed.
        mPrefs[PREF_DISPLAYMODE] = ReadInt(iniReader, "Display", "DisplayMode", 2, 1, 3);

        // Both axes or neither; the ceiling is the largest square a D3D9 render target can describe.
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

        mPrefs[PREF_SCALINGFILTER] = ReadInt(iniReader, "Display", "ScalingFilter", 1, 0, 1);
        mPrefs[PREF_FPSCOUNTER] = ReadInt(iniReader, "Display", "FpsCounter", 0, 0, 1);
        mPrefs[PREF_MAXFRAMERATE] = ReadInt(iniReader, "Display", "MaxFrameRate", 1, 0, 9999);
    }

    static void ReadGraphics(CIniReader& iniReader)
    {
        mPrefs[PREF_UTILISATION] = ReadInt(iniReader, "Graphics", "ImprovedUtilisation", 1, 0, 1);
        mPrefs[PREF_ANISOTROPICFILTERING] = ReadInt(iniReader, "Graphics", "AnisotropicFiltering", 16, 0, 16);
        mPrefs[PREF_NORIMLIGHTING] = ReadInt(iniReader, "Graphics", "NoRimLighting", 0, 0, 1);
        mPrefs[PREF_NOSPRINTAIMBLUR] = ReadInt(iniReader, "Graphics", "NoSprintAimBlur", 0, 0, 1);
        mPrefs[PREF_SATURATION] = std::clamp(iniReader.ReadFloat("Graphics", "Saturation", 0.5f), 0.0f, 4.0f);
        mPrefs[PREF_VOLUMETRICFOG] = ReadInt(iniReader, "Graphics", "VolumetricFog", 0, 0, 1);
        mPrefs[PREF_VOLUMETRICCLOUDS] = ReadInt(iniReader, "Graphics", "VolumetricClouds", 0, 0, 1);

        mPrefs[PREF_BEYONDULTRAGEOMETRY] = ReadInt(iniReader, "BeyondUltra", "BeyondUltraGeometry", 4, 0, 4);
        mPrefs[PREF_BEYONDULTRASHADOWS] = ReadInt(iniReader, "BeyondUltra", "BeyondUltraShadows", 1, 0, 1);

        // Not clamped: the point is to go past the presets, and the device decides.
        mPrefs[PREF_SHADOWRESOLUTION] = iniReader.ReadInteger("BeyondUltra", "ShadowResolution", 2048);

        mPrefs[PREF_BEYONDULTRATERRAIN] = ReadInt(iniReader, "BeyondUltra", "BeyondUltraTerrain", 3, 0, 3);
    }

    static void ReadGameplay(CIniReader& iniReader)
    {
        mPrefs[PREF_MOUSESPEEDCAP] = ReadInt(iniReader, "Gameplay", "RemoveMouseSpeedCap", 1, 0, 1);
        mPrefs[PREF_AIMTOGGLE] = ReadInt(iniReader, "Gameplay", "AimToggle", 0, 0, 1);
        mPrefs[PREF_SPRINTTOGGLE] = ReadInt(iniReader, "Gameplay", "SprintToggle", 0, 0, 1);
        mPrefs[PREF_LIMITEDSAVING] = ReadInt(iniReader, "Gameplay", "LimitedSaving", 0, 0, 1);
        mPrefs[PREF_CONSOLEAUTOSAVES] = ReadInt(iniReader, "Gameplay", "ConsoleAutosaves", 1, 0, 1);
        mPrefs[PREF_NOBLINKINGITEMS] = ReadInt(iniReader, "Gameplay", "NoBlinkingItems", 0, 0, 1);
        mPrefs[PREF_NOHITINDICATOR] = ReadInt(iniReader, "Gameplay", "NoHitIndicator", 0, 0, 1);
        mPrefs[PREF_CUSTOMBINDS] = ReadInt(iniReader, "Gameplay", "CustomBinds", 1, 0, 1);

        // -2 never shows the HUD, -1 never fades it, 0 keeps the game's own delay, above that seconds.
        mPrefs[PREF_HUDFADEOUT] = ReadInt(iniReader, "Gameplay", "HudFadeOut", 0, -2, 60);

        mPrefs[PREF_MOUSELOOKSENSITIVITY] =
            ReadOptionalFloat(iniReader, "Gameplay", "MouseLookSensitivity", 1.0f, 0.01f, 5.0f);
        mPrefs[PREF_SPRINTTURNMODIFIER] =
            ReadOptionalFloat(iniReader, "Gameplay", "SprintTurnModifier", 0.0f, 0.01f, 2.0f);
    }

    static void ReadFieldOfView(CIniReader& iniReader)
    {
        mPrefs[PREF_FIELDOFVIEW] = std::clamp(iniReader.ReadFloat("FieldOfView", "FieldOfView", 75.0f), 45.0f, 140.0f);
        mPrefs[PREF_VIEWMODELFIELDOFVIEW] =
            std::clamp(iniReader.ReadFloat("FieldOfView", "ViewmodelFieldOfView", 75.0f), 45.0f, 140.0f);
        mPrefs[PREF_IRONSIGHTFIELDOFVIEW] =
            ReadOptionalFloat(iniReader, "FieldOfView", "IronsightFieldOfView", 0.0f, 20.0f, 140.0f);
        mPrefs[PREF_VEHICLEFIELDOFVIEW] =
            ReadOptionalFloat(iniReader, "FieldOfView", "VehicleFieldOfView", 0.0f, 45.0f, 140.0f);
    }

    // After ReadGameplay: the controller aim toggle defaults to the mouse one.
    static void ReadController(CIniReader& iniReader)
    {
        mPrefs[PREF_CONTROLLERLOOKSENSITIVITY] =
            ReadOptionalFloat(iniReader, "Controller", "ControllerLookSensitivity", 1.0f, 0.01f, 5.0f);
        mPrefs[PREF_AIMASSIST] = ReadInt(iniReader, "Controller", "AimAssist", 1, 0, 1);
        mPrefs[PREF_VIBRATION] = ReadInt(iniReader, "Controller", "Vibration", 1, 0, 1);
        mPrefs[PREF_CONTROLLERPROMPTS] = ReadInt(iniReader, "Controller", "ControllerPrompts", 1, 0, 1);

        const auto nAimToggle = std::get<int32_t>(mPrefs[PREF_AIMTOGGLE]);
        mPrefs[PREF_AIMTOGGLECONTROLLER] = ReadInt(iniReader, "Controller", "AimToggle", nAimToggle, 0, 1);
    }

    static void ReadContentUnlocks(CIniReader& iniReader)
    {
        mPrefs[PREF_PREDECESSORTAPES] = ReadInt(iniReader, "ContentUnlocks", "PredecessorTapesUnlock", 1, 0, 1);
        mPrefs[PREF_MACHETES] = ReadInt(iniReader, "ContentUnlocks", "MachetesUnlock", 1, 0, 1);
    }

    static void ReadDebug(CIniReader& iniReader)
    {
        mPrefs[PREF_DEBUGINVINCIBILITY] = ReadInt(iniReader, "Debug", "Invincibility", 0, 0, 1);
        mPrefs[PREF_DEBUGINFINITEAMMO] = ReadInt(iniReader, "Debug", "InfiniteAmmo", 0, 0, 1);
        mPrefs[PREF_DEBUGUNLOCKALLWEAPONS] = ReadInt(iniReader, "Debug", "UnlockAllWeapons", 0, 0, 1);
        mPrefs[PREF_DEBUGDIAMONDS] = ReadInt(iniReader, "Debug", "Diamonds", 0, 0, 999);
        mPrefs[PREF_DEBUGNOCLIP] = ReadInt(iniReader, "Debug", "Noclip", 0, 0, 1);
        mPrefs[PREF_DEBUGFREECAM] = ReadInt(iniReader, "Debug", "Freecam", 0, 0, 1);
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
    }

    // Session choices go back on top where the file has not moved. Done here rather than in the
    // caller, so the watcher's re-reads keep them too.
    static void ReapplyHolds()
    {
        for (size_t i = 0; i < static_cast<size_t>(Pref::COUNT); i++)
        {
            if (!mHeld[i])
                continue;

            if (mPrefs[i] == mHeldFile[i])
                mPrefs[i] = mHeldValue[i];
            else
                mHeld[i] = false;
        }
    }

    static void OnIniChanged(const std::string&, const filewatch::Event change_type)
    {
        if (change_type != filewatch::Event::modified || bJackalFixWritingIni)
            return;

        ReadIniSettings();
        JackalFix::onIniFileChange().executeAll();
    }

    static void EnsureIniWatch(CIniReader& iniReader)
    {
        static std::once_flag flag;
        std::call_once(flag, [&]()
        {
            if (!std::filesystem::exists(iniReader.GetIniPath()))
                return;

            static filewatch::FileWatch<std::string> watch(iniReader.GetIniPath().string(), OnIniChanged);
        });
    }

public:
    static inline void ReadIniSettings()
    {
        CIniReader iniReader("");
        ReadGeneral(iniReader);
        ReadDisplay(iniReader);
        ReadGraphics(iniReader);
        ReadGameplay(iniReader);
        ReadFieldOfView(iniReader);
        ReadController(iniReader);
        ReadContentUnlocks(iniReader);
        ReadDebug(iniReader);

        // Before the holds go back on, so this is the file and nothing else.
        mFile = mPrefs;

        ReapplyHolds();
        EnsureIniWatch(iniReader);
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
