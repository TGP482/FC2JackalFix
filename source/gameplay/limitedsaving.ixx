module;

#include <common.hxx>

export module limitedsaving;

import common;
import dunia;
import settings;

static bool bLimitedSaving = false;

// Head of CFCXPauseMenuPage::Open; the string pointer in the middle is masked, being absolute.
static const char* const szPauseOpenPattern =
    "83 EC 5C 53 55 56 57 8D 44 24 13 8B F1 50 8D 4C 24 20 33 DB BD ? ? ? ? BF 0F 00 00 00";

static constexpr ptrdiff_t nPauseSaveEntry = 0x1EC; // the save entry's item index, -1 when skipped
static constexpr ptrdiff_t nPageShown = 0x164;      // set by the page enter, cleared by the leave
static constexpr size_t nBuildEntriesSlot = 15;
static constexpr size_t nClearEntriesSlot = 16;

using PageMethod_t = void(__fastcall*)(void* pPage, void* pEdx);

class LimitedSaving
{
public:
    LimitedSaving()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            BindBool(bLimitedSaving, PREF_LIMITEDSAVING);

            // Shared entry block shape; skipping the save block leaves the index at -1, which the
            // engine already handles.
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

                // Both references are internal, so the patch is position-independent.
                static raw_mem fnPauseSaveEntry(match.get<void>(0), {
                    0xC7, 0x86, 0xEC, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                    0xE9, 0x86, 0x00, 0x00, 0x00,
                });

                BindPatch(fnPauseSaveEntry, PREF_LIMITEDSAVING);

                // Only worth installing alongside the patch above. The page is built once and
                // kept, so its entries are cleared and rebuilt on every open.
                if (auto* pPauseOpen = dunia_find(szPauseOpenPattern))
                {
                    static auto PauseOpenHook = safetyhook::create_mid(pPauseOpen, [](SafetyHookContext& regs)
                    {
                        auto pPage = reinterpret_cast<uint8_t*>(regs.ecx);
                        if (pPage == nullptr)
                            return;

                        // The clear declines while the page's document is down, and a build on a
                        // clear that did not happen is the whole menu twice.
                        if (*reinterpret_cast<uint8_t*>(pPage + nPageShown) == 0)
                            return;

                        auto bEntryGone = *reinterpret_cast<int32_t*>(pPage + nPauseSaveEntry) == -1;
                        if (bEntryGone == bLimitedSaving)
                            return;

                        // Ahead of the stock open, which walks the entries into the list widget.
                        auto ppVTable = *reinterpret_cast<uintptr_t**>(pPage);
                        reinterpret_cast<PageMethod_t>(ppVTable[nClearEntriesSlot])(pPage, nullptr);
                        reinterpret_cast<PageMethod_t>(ppVTable[nBuildEntriesSlot])(pPage, nullptr);
                    });
                }

                break;
            }

            // F5/F9 refused where they are acted on: the bindings are read once at startup, so
            // unbinding them would fix the setting for the run. Quicksave asks the save manager's
            // vtable +50h whether saving is allowed; quickload skips out on a null manager fetch.
            static constexpr ptrdiff_t nQuickSaveAllowed = 44; // test al,al, on the answer

            if (auto* pQuickSave = dunia_find(
                "3B 05 ? ? ? ? 0F 85 ? ? ? ? 8B 0D ? ? ? ? 6A 01 E8 ? ? ? ? 8B F0 85 F6 0F 84 ? ? ? ? "
                "8B 06 8B 50 50 8B CE FF D2 84 C0", nQuickSaveAllowed))
            {
                static auto QuickSaveHook = safetyhook::create_mid(pQuickSave,
                    [](SafetyHookContext& regs)
                    {
                        if (!bLimitedSaving)
                            return;

                        // Answer the allowed test with a no.
                        regs.eax &= ~static_cast<uintptr_t>(0xFF);
                    });
            }

            static constexpr ptrdiff_t nQuickLoadManager = 21; // test eax,eax, on the fetched manager

            if (auto* pQuickLoad = dunia_find(
                "3B 05 ? ? ? ? 75 32 8B 0D ? ? ? ? 6A 01 E8 ? ? ? ? 85 C0 74 15 83 B8 44 01 00 00 00",
                nQuickLoadManager))
            {
                static auto QuickLoadHook = safetyhook::create_mid(pQuickLoad,
                    [](SafetyHookContext& regs)
                    {
                        if (!bLimitedSaving)
                            return;

                        // Never dereferenced here; the branch falls to its return.
                        regs.eax = 0;
                    });
            }
        };
    }
} LimitedSaving;
