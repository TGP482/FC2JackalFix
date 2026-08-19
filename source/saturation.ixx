module;

#include <common.hxx>
#include <atomic>

export module saturation;

import common;
import dunia;
import settings;

// Offset of fSaturation within the bloom settings struct the pass is handed.
static constexpr uint32_t nSettingsSaturation = 0x3C;

// The first stack argument, past the return address the entry has not yet consumed.
static constexpr uint32_t nFirstStackArgument = 4;

// The pass runs on the render thread while the watch fires on the file watcher's, so the value is
// read atomically.
static std::atomic<float> fSaturation{ 0.5f };

class Saturation
{
public:
    Saturation()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // The adaptive bloom pass. Entry through the frame prologue, with the call
            // displacement and the one absolute address wildcarded.
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
