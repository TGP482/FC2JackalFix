module;

#include <common.hxx>
#include <FileWatch.hpp>
#include <variant>

export module settings;

import common;

export enum Pref
{
    PREF_SKIPINTRO,
    PREF_SKIPTITLESCREEN,
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
    PREF_HIGHPRECISIONTIMER,
    PREF_SKIPSYSTEMDETECTION,
    PREF_LARGEADDRESSAWARE,
    PREF_MAXFRAMERATE,
    PREF_ANISOTROPICFILTERING,
    PREF_X360GAMMA,
    PREF_ENHANCEDLODS,
    PREF_ENHANCEDSHADOWRANGE,
    PREF_SHADOWRESOLUTION,
    PREF_BORDERLESS,
    PREF_CPUAFFINITY,
    PREF_LIMITEDSAVING,
    PREF_CONSOLEAUTOSAVES,
    PREF_NOBLINKINGITEMS,
    PREF_PREDECESSORTAPES,
    PREF_MACHETES,

    COUNT,
};

export class CSettings
{
private:
    static inline std::array<std::variant<int32_t, float, std::string>, static_cast<size_t>(Pref::COUNT)> mPrefs;

public:
    static inline void ReadIniSettings()
    {
        CIniReader iniReader("");
        mPrefs[PREF_SKIPINTRO] = std::clamp(iniReader.ReadInteger("General", "SkipIntro", 1), 0, 1);
        mPrefs[PREF_SKIPTITLESCREEN] = std::clamp(iniReader.ReadInteger("General", "SkipTitleScreen", 1), 0, 1);
        mPrefs[PREF_CPUAFFINITY] = std::clamp(iniReader.ReadInteger("General", "CpuAffinity", 0), 0, 64);
        mPrefs[PREF_HIGHPRECISIONTIMER] = std::clamp(iniReader.ReadInteger("General", "HighPrecisionTimer", 1), 0, 1);
        mPrefs[PREF_SKIPSYSTEMDETECTION] = std::clamp(iniReader.ReadInteger("General", "SkipSystemDetection", 1), 0, 1);
        mPrefs[PREF_LARGEADDRESSAWARE] = std::clamp(iniReader.ReadInteger("General", "LargeAddressAware", 0), 0, 1);

        mPrefs[PREF_BORDERLESS] = std::clamp(iniReader.ReadInteger("Display", "Borderless", 0), 0, 1);

        // 0 is not uncapped. The engine uses 9999+ to disable the frame limiter.
        auto nMaxFrameRate = std::clamp(iniReader.ReadInteger("Display", "MaxFrameRate", 60), 0, 9999);
        mPrefs[PREF_MAXFRAMERATE] = nMaxFrameRate < 1 ? 9999 : nMaxFrameRate;

        mPrefs[PREF_ANISOTROPICFILTERING] = std::clamp(iniReader.ReadInteger("Graphics", "AnisotropicFiltering", 16), 0, 16);
        mPrefs[PREF_X360GAMMA] = std::clamp(iniReader.ReadInteger("Graphics", "Xbox360Gamma", 0), 0, 1);

        mPrefs[PREF_ENHANCEDLODS] = std::clamp(iniReader.ReadInteger("Graphics", "EnhancedLODs", 1), 0, 1);
        mPrefs[PREF_ENHANCEDSHADOWRANGE] = std::clamp(iniReader.ReadInteger("Graphics", "EnhancedShadowRange", 1), 0, 1);

        mPrefs[PREF_SHADOWRESOLUTION] = std::clamp(iniReader.ReadInteger("Graphics", "ShadowResolution", 2048), 128, 2560);

        mPrefs[PREF_MOUSESPEEDCAP] = std::clamp(iniReader.ReadInteger("Gameplay", "RemoveMouseSpeedCap", 1), 0, 1);

        auto nAimToggle = std::clamp(iniReader.ReadInteger("Gameplay", "AimToggle", 0), 0, 1);
        auto nSprintToggle = std::clamp(iniReader.ReadInteger("Gameplay", "SprintToggle", 0), 0, 1);
        mPrefs[PREF_AIMTOGGLE] = nAimToggle;
        mPrefs[PREF_SPRINTTOGGLE] = nSprintToggle;

        mPrefs[PREF_LIMITEDSAVING] = std::clamp(iniReader.ReadInteger("Gameplay", "LimitedSaving", 0), 0, 1);
        mPrefs[PREF_CONSOLEAUTOSAVES] = std::clamp(iniReader.ReadInteger("Gameplay", "ConsoleAutosaves", 1), 0, 1);
        mPrefs[PREF_NOBLINKINGITEMS] = std::clamp(iniReader.ReadInteger("Gameplay", "NoBlinkingItems", 0), 0, 1);

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

        mPrefs[PREF_AIMTOGGLECONTROLLER] = std::clamp(iniReader.ReadInteger("Controller", "AimToggle", nAimToggle), 0, 1);

        mPrefs[PREF_PREDECESSORTAPES] = std::clamp(iniReader.ReadInteger("ContentUnlocks", "PredecessorTapesUnlock", 1), 0, 1);
        mPrefs[PREF_MACHETES] = std::clamp(iniReader.ReadInteger("ContentUnlocks", "MachetesUnlock", 1), 0, 1);

        static std::once_flag flag;
        std::call_once(flag, [&]()
        {
            if (std::filesystem::exists(iniReader.GetIniPath()))
            {
                static filewatch::FileWatch<std::string> watch(iniReader.GetIniPath().string(), [](const std::string&, const filewatch::Event change_type)
                {
                    if (change_type == filewatch::Event::modified)
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
} JackalFixSettings;
