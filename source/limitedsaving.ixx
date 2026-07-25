module;

#include <common.hxx>

export module limitedsaving;

import common;
import dunia;
import settings;

// PC save-anywhere features are not controlled by a runtime setting. The pause menu save entry
// and quicksave bindings require separate interventions.
//
// The pause menu references CSaveGamePage through its descriptor pointer (0x108541A8), so
// removing the class name alone does not disable the entry.

static bool bLimitedSaving = false;

class LimitedSaving
{
public:
    LimitedSaving()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            bLimitedSaving = JackalFixSettings.GetInt(PREF_LIMITEDSAVING) != 0;

            JackalFix::onIniFileChange() += []()
            {
                bLimitedSaving = JackalFixSettings.GetInt(PREF_LIMITEDSAVING) != 0;
            };

            // Pause menu entries are built from the same block shape. Skipping the save entry block leaves its
            // item index at -1, which the engine already handles at 0x10857728.
            auto pattern = dunia_pattern(
                "53 6A 7C E8 ? ? ? ? 8B F8 83 C4 08 3B FB 74 23 39 1D ? ? ? ? 75 07 33 C9 E8 ? ? ? ? "
                "6A 01 53 68 ? ? ? ? 56 8B CF E8 ? ? ? ? 8B F8 EB 02 33 FF 68 ? ? ? ? 8D 4C 24 20 "
                "E8 ? ? ? ? 68 ? ? ? ? 8D 4C 24 3C");

            if (pattern.empty())
                return;

            for (size_t i = 0; i < pattern.size(); i++)
            {
                auto match = pattern.get(i);
                auto pLabel = *match.get<const char*>(0x37);

                if (!pLabel || _stricmp(pLabel, "PAUSE_SAVEGAME") != 0)
                    continue;

                // Set the entry index to -1 and skip the block. Both references are internal, so the patch is
                // position-independent.
                static raw_mem fnPauseSaveEntry(match.get<void>(0), {
                    0xC7, 0x86, 0xEC, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                    0xE9, 0x86, 0x00, 0x00, 0x00,
                });

                static auto LimitedSavingCB = []()
                {
                    if (bLimitedSaving)
                        fnPauseSaveEntry.Write();
                    else
                        fnPauseSaveEntry.Restore();
                };

                LimitedSavingCB();

                JackalFix::onIniFileChange() += []()
                {
                    LimitedSavingCB();
                };

                break;
            }

            // F5/F9 bindings are defined in XML, not the DLL. Clearing the signal during parsing makes the
            // binding behave like one without a signal, matching a removed XML entry.
            pattern = dunia_pattern("83 EC 38 53 55 56 57 8B 7C 24 4C 8B E9 8B 0F 8B 01 8B 90 88 00 00 00 68 ? ? ? ? FF D2 8B F0");
            if (pattern.empty())
                return;

            static auto BindingSignalHook = safetyhook::create_mid(pattern.get_first(0x1E), [](SafetyHookContext& regs)
            {
                if (!bLimitedSaving)
                    return;

                auto pSignal = (const char*)regs.eax;
                if (!pSignal)
                    return;

                if (_stricmp(pSignal, "quicksave") == 0 || _stricmp(pSignal, "quickload") == 0)
                    regs.eax = 0;
            });

        };
    }
} LimitedSaving;
