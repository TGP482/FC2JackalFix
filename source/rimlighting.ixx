/*
  "Removed rim lighting" from Boggalog's Far Cry 2 Patched, in code.
*/

module;

#include <common.hxx>
#include <atomic>

export module rimlighting;

import common;
import dunia;
import settings;

// Offsets from the render frame context this call is made on.
static constexpr uint32_t nProviderPointer = 0x60;      // CGlobalShaderParameterProvider*
static constexpr uint32_t nRimLightIntensityValue = 0xD0; // slot +0xC0, value float4 at slot +0x10

static std::atomic<bool> bRemoveRimLighting{ false };

// __fastcall with no stack arguments and no callee cleanup.
static SafetyHookInline FillShaderParametersHook{};

static void __fastcall FillShaderParameters(void* pFrame, void* pEdx)
{
    // Original first: it computes the value, sets the slot's dirty bits and stores. Overwriting
    // afterwards keeps the dirty bits it just set.
    FillShaderParametersHook.fastcall(pFrame, pEdx);

    if (!bRemoveRimLighting.load(std::memory_order_relaxed) || pFrame == nullptr)
        return;

    auto* pProvider = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pFrame) + nProviderPointer);
    if (pProvider == nullptr)
        return;

    *reinterpret_cast<float*>(pProvider + nRimLightIntensityValue) = 0.0f;
}

class RimLighting
{
public:
    RimLighting()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Per frame global shader parameter fill. Entry through the frame prologue, with the
            // two call displacements and the one absolute global wildcarded.
            auto pattern = dunia_pattern("55 8B EC 83 E4 F0 83 EC 24 53 56 57 8B F1 E8 ? ? ? ? 8B 0D ? ? ? ? 8B F8 89 7C 24 18 E8 ? ? ? ? F3 0F 10 57 10");
            if (pattern.empty())
                return;

            // The hook goes in whatever the setting says, so the toggle can be flipped live.
            FillShaderParametersHook = safetyhook::create_inline(pattern.get_first(), FillShaderParameters);

            static auto RimLightingCB = []()
            {
                bRemoveRimLighting.store(JackalFixSettings.GetInt(PREF_NORIMLIGHTING) != 0, std::memory_order_relaxed);
            };

            RimLightingCB();

            // One store per frame, so a change takes effect on the next one.
            JackalFix::onIniFileChange() += []()
            {
                RimLightingCB();
            };
        };
    }
} RimLighting;
