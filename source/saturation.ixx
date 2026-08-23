module;

#include <common.hxx>
#include <atomic>

export module saturation;

import common;
import dunia;
import settings;

// fSaturation in the bloom settings struct the pass is handed.
static constexpr uint32_t nSettingsSaturation = 0x3C;

// First stack arg, past the return address.
static constexpr uint32_t nFirstStackArgument = 4;

// Render thread reads, file watcher writes.
static std::atomic<float> fSaturation{ 0.5f };

class Saturation
{
public:
    Saturation()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Adaptive bloom pass prologue; call displacement and one absolute wildcarded.
            auto* pBloom = dunia_find("55 8B EC 83 E4 F0 81 EC 44 01 00 00 53 56 57 8B F1 E8 ? ? ? ? 8B 8E 90 00 00 00 6A 00 68 ? ? ? ?");
            if (!pBloom)
                return;

            static auto SaturationHook = safetyhook::create_mid(pBloom, [](SafetyHookContext& regs)
            {
                auto* pSettings = *reinterpret_cast<uint8_t**>(regs.esp + nFirstStackArgument);
                if (pSettings == nullptr)
                    return;

                *reinterpret_cast<float*>(pSettings + nSettingsSaturation) = fSaturation.load(std::memory_order_relaxed);
            });

            BindFloat(fSaturation, PREF_SATURATION);
        };
    }
} Saturation;
