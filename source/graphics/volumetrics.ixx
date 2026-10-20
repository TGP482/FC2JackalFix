module;

#include <common.hxx>
#include <atomic>
#include <cmath>
#include <cstring>

export module volumetrics;

import common;
import dunia;
import settings;
import volumetricclouds;
import volumetricdevice;
import volumetricshaders;

// Volumetric fog ported from the Far Cry Instincts recomp, with the clouds (volumetricclouds.ixx)
// drawn just before it. Both run inside the scene renders (FUN_10342360, the one gameplay runs, and
// FUN_10355F60) once the opaque passes and the sky are done, just before the BlendedNoWater pass
// (0x20), so particles and water still land on top. That pass carries the scene colour target and
// the main viewport parameter provider, whose DepthVPSampler is the view depth the soft particles
// read. Camera, fog and depth unpacking come from the provider, the sun and moon from the time of day
// state the sky render fetches (FUN_1033E9E0), so the fog follows the day and night cycle and the
// weather. No sun shadow map is read, so there are no light shafts.

// Scene pass, as FUN_103671D0 sets it up and FUN_1039D900 runs it.
static constexpr uintptr_t nPassTarget = 0x24;
static constexpr uintptr_t nPassProvider = 0x6C;

// CViewportShaderParameterProvider, each value 0x10 past the slot FUN_103788F0 registers.
static constexpr uintptr_t nViewPoint = 0x2B0;
static constexpr uintptr_t nCameraRight = 0x310;
static constexpr uintptr_t nCameraDirection = 0x330;
static constexpr uintptr_t nCameraUp = 0x350;
static constexpr uintptr_t nCameraDistances = 0x370;      // near, far
static constexpr uintptr_t nCameraNearPlaneSize = 0x390;  // width and height at the near plane
static constexpr uintptr_t nFogColorVector = 0x3B0;
static constexpr uintptr_t nFogColor = 0x3D0;
static constexpr uintptr_t nFogColorRange = 0x3F0;
static constexpr uintptr_t nFogValues = 0x410;
static constexpr uintptr_t nFogHeightValues = 0x430;
static constexpr uintptr_t nUncompressDepthWeightsWS = 0x650;
static constexpr uintptr_t nDepthVPSampler = 0x4AC;       // engine texture
static constexpr uintptr_t nBloomAdaptationFactor = 0x670;
static constexpr size_t nProviderSize = 0x700;

// Time of day state; directions point at the sun and moon, the shaders get them negated (FUN_103DC3C0).
static constexpr uintptr_t nSunDirection = 0x148;
static constexpr uintptr_t nSunColor = 0x160;
static constexpr uintptr_t nMoonDirection = 0x194;
static constexpr uintptr_t nMoonColor = 0x1A0;
static constexpr size_t nEnvironmentSize = 0x240;

// Packed depth reads 1.0 on the sky; the float resolve sits at the far plane there.
static constexpr float fPackedSkyShare = 0.998f;
static constexpr float fFloatSkyShare = 0.98f;

enum class Renderer
{
    None,
    DX9,
    DX10,
};

struct Float4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Light
{
    Float4 dir;
    Float4 color;
};

static std::atomic<bool> bVolumetricFog{ false };
static Renderer eRenderer = Renderer::None;
static uintptr_t nEnvironment = 0;

static Float4 ReadFloat4(uintptr_t nAddress)
{
    Float4 value;
    std::memcpy(&value, reinterpret_cast<const void*>(nAddress), sizeof(value));
    return value;
}

static void Store(float (&slot)[4], Float4 value)
{
    slot[0] = value.x;
    slot[1] = value.y;
    slot[2] = value.z;
    slot[3] = value.w;
}

static Float4 Mix(Float4 a, Float4 b, float t)
{
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
}

static Float4 Normalised(Float4 v)
{
    const float fLength = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (fLength < 1e-6f)
        return { 0.0f, 0.0f, -1.0f, 0.0f };
    return { v.x / fLength, v.y / fLength, v.z / fLength, 0.0f };
}

static VolumetricDevice* ActiveDevice()
{
    switch (eRenderer)
    {
    case Renderer::DX9:
        return &VolumetricDeviceDX9();
    case Renderer::DX10:
        return &VolumetricDeviceDX10();
    default:
        return nullptr;
    }
}

// The sun until it sets, then the moon.
static Light ReadLight()
{
    if (!Readable(nEnvironment, nEnvironmentSize))
        return {};

    const auto toSun = ReadFloat4(nEnvironment + nSunDirection);
    const auto toMoon = ReadFloat4(nEnvironment + nMoonDirection);
    const float fSun = std::clamp(0.5f + toSun.z * 10.0f, 0.0f, 1.0f);
    const auto toLight = Normalised(Mix(toMoon, toSun, fSun));

    Light light;
    light.dir = { -toLight.x, -toLight.y, -toLight.z, 0.0f };
    light.color = Mix(ReadFloat4(nEnvironment + nMoonColor), ReadFloat4(nEnvironment + nSunColor), fSun);
    return light;
}

static Float4 DepthUnpack(uintptr_t nProvider, bool bPacked, float fFar)
{
    if (!bPacked)
        return { 1.0f, 0.0f, 0.0f, fFar * fFloatSkyShare };

    auto weights = ReadFloat4(nProvider + nUncompressDepthWeightsWS);
    weights.w = fPackedSkyShare * (weights.x + weights.y + weights.z);
    return weights;
}

// False while the camera is not set up.
static bool BuildConstants(uintptr_t nProvider, VolSize scene, bool bPacked, VolConstants& constants)
{
    const auto distances = ReadFloat4(nProvider + nCameraDistances);
    const auto nearPlane = ReadFloat4(nProvider + nCameraNearPlaneSize);
    if (distances.x <= 0.0f || nearPlane.x <= 0.0f || nearPlane.y <= 0.0f || scene.w == 0 || scene.h == 0)
        return false;

    const auto light = ReadLight();
    const Float4 size = { float(scene.w), float(scene.h), float((scene.w + 1) / 2), float((scene.h + 1) / 2) };
    const Float4 projection = { 2.0f * distances.x / nearPlane.x, 2.0f * distances.x / nearPlane.y, distances.x,
                                distances.y };

    Store(constants.c[kRegSize], size);
    Store(constants.c[kRegProj], projection);
    Store(constants.c[kRegEye], ReadFloat4(nProvider + nViewPoint));
    Store(constants.c[kRegRight], ReadFloat4(nProvider + nCameraRight));
    Store(constants.c[kRegUp], ReadFloat4(nProvider + nCameraUp));
    Store(constants.c[kRegForward], ReadFloat4(nProvider + nCameraDirection));
    Store(constants.c[kRegDepth], DepthUnpack(nProvider, bPacked, distances.y));
    Store(constants.c[kRegFog], ReadFloat4(nProvider + nFogValues));
    Store(constants.c[kRegFogHeight], ReadFloat4(nProvider + nFogHeightValues));
    Store(constants.c[kRegFogColor], ReadFloat4(nProvider + nFogColor));
    Store(constants.c[kRegFogColorVector], ReadFloat4(nProvider + nFogColorVector));
    Store(constants.c[kRegFogColorRange], ReadFloat4(nProvider + nFogColorRange));
    Store(constants.c[kRegExposure], ReadFloat4(nProvider + nBloomAdaptationFactor));
    Store(constants.c[kRegLightDir], light.dir);
    Store(constants.c[kRegLightColor], light.color);
    return true;
}

// Marched at half resolution, blurred both ways, then laid over the scene.
static void RunFog(VolumetricDevice& device, VolConstants constants)
{
    const auto scene = device.SceneSize();
    if (!device.EnsureTarget(VolTexture::FogA, scene) || !device.EnsureTarget(VolTexture::FogB, scene))
        return;

    const float fTexelW = 1.0f / constants.c[kRegSize][2];
    const float fTexelH = 1.0f / constants.c[kRegSize][3];

    device.Draw({ VolShader::FogMarch, VolTexture::None, VolTexture::FogA, VolBlend::Replace }, constants);

    Store(constants.c[kRegBlur], { fTexelW, 0.0f, 0.0f, 0.0f });
    device.Draw({ VolShader::FogBlur, VolTexture::FogA, VolTexture::FogB, VolBlend::Replace }, constants);

    Store(constants.c[kRegBlur], { 0.0f, fTexelH, 0.0f, 0.0f });
    device.Draw({ VolShader::FogBlur, VolTexture::FogB, VolTexture::FogA, VolBlend::Replace }, constants);

    device.Draw({ VolShader::FogComposite, VolTexture::FogA, VolTexture::None, VolBlend::Over }, constants);
}

static void RenderVolumetrics(uintptr_t nPass)
{
    const bool bFog = bVolumetricFog.load(std::memory_order_relaxed);
    const bool bClouds = VolumetricCloudsEnabled();
    if (!bFog && !bClouds)
        return;

    auto* pDevice = ActiveDevice();
    const auto nProvider = ReadPointer<uintptr_t>(nPass + nPassProvider);
    if (!pDevice || !Readable(nProvider, nProviderSize))
        return;

    const auto nSceneTarget = ReadPointer<uintptr_t>(nPass + nPassTarget);
    const auto nDepthTexture = ReadPointer<uintptr_t>(nProvider + nDepthVPSampler);
    if (!nSceneTarget || !nDepthTexture || !pDevice->Begin(nSceneTarget, nDepthTexture))
        return;

    VolConstants constants;
    if (BuildConstants(nProvider, pDevice->SceneSize(), pDevice->DepthPacked(), constants))
    {
        if (bClouds)
            RunVolumetricClouds(*pDevice, constants, nEnvironment);
        if (bFog)
            RunFog(*pDevice, constants);
    }

    pDevice->End();
}

class Volumetrics
{
public:
    Volumetrics()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // FUN_1037A150, the sky render, right after it fetches the time of day state.
            auto* pSkyEnvironment = dunia_find("8B 0D ? ? ? ? E8 ? ? ? ? 8B F0 85 F6 0F 84 ? ? ? ? 80 7E 14 00", 11);

            // BlendedNoWater fetched and about to run. FUN_10342360 runs it right after the sky pass;
            // FUN_10355F60 after its debug depth dump.
            auto* pGameplayBlended = dunia_find(
                "68 80 00 00 00 E8 ? ? ? ? 8B C8 E8 ? ? ? ? 8B 46 14 8B 48 04 6A 20 E8 ? ? ? ? 8B C8 E8", 30);
            auto* pResolveBlended = dunia_find(
                "FF D0 8B 46 14 8B 48 04 6A 20 E8 ? ? ? ? 8B C8 E8 ? ? ? ? 8B 46 14 8B 48 04 6A 21", 15);

            if (!pSkyEnvironment || !pGameplayBlended || !pResolveBlended)
                return;

            static auto SkyEnvironmentHook = safetyhook::create_mid(pSkyEnvironment, [](SafetyHookContext& regs)
            {
                nEnvironment = regs.eax;
            });

            static auto GameplayBlendedHook = safetyhook::create_mid(pGameplayBlended, [](SafetyHookContext& regs)
            {
                RenderVolumetrics(regs.eax);
            });

            static auto ResolveBlendedHook = safetyhook::create_mid(pResolveBlended, [](SafetyHookContext& regs)
            {
                RenderVolumetrics(regs.eax);
            });

            BindBool(bVolumetricFog, PREF_VOLUMETRICFOG);
        };

        // The environment fetched this frame only; a level change frees it.
        JackalFix::onFrameEndDX9Event() += [](void*)
        {
            eRenderer = Renderer::DX9;
            nEnvironment = 0;
        };

        JackalFix::onFrameEndDX10Event() += [](void*, void*)
        {
            eRenderer = Renderer::DX10;
            nEnvironment = 0;
        };

        JackalFix::onDeviceResetEvent() += []()
        {
            VolumetricDeviceDX9().Release();
        };
    }
} Volumetrics;
