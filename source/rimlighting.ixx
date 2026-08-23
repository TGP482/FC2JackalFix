/* "Removed rim lighting" from Boggalog's Far Cry 2 Patched. */

module;

#include <common.hxx>
#include <atomic>

export module rimlighting;

import common;
import dunia;
import settings;

// Offsets from the render frame context.
static constexpr uint32_t nProviderPointer = 0x60;      // CGlobalShaderParameterProvider*
static constexpr uint32_t nRimLightIntensityValue = 0xD0; // slot +0xC0, value float4 at slot +0x10

static std::atomic<bool> bRemoveRimLighting{ false };

// No stack args, no callee cleanup.
static SafetyHookInline FillShaderParametersHook{};

static void __fastcall FillShaderParameters(void* pFrame, void* pEdx)
{
    // Original first: overwriting after keeps the dirty bits it set.
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
            // Per-frame shader parameter fill; call displacements and one absolute wildcarded.
            auto* pFill = dunia_find("55 8B EC 83 E4 F0 83 EC 24 53 56 57 8B F1 E8 ? ? ? ? 8B 0D ? ? ? ? 8B F8 89 7C 24 18 E8 ? ? ? ? F3 0F 10 57 10");
            if (!pFill)
                return;

            // Installed unconditionally so the toggle flips live.
            FillShaderParametersHook = safetyhook::create_inline(pFill, FillShaderParameters);

            BindBool(bRemoveRimLighting, PREF_NORIMLIGHTING);
        };
    }
} RimLighting;
