module;

#include <common.hxx>

export module limitedsaving;

import common;
import dunia;
import settings;

// No runtime setting gates PC save-anywhere, so the pause menu entry and the quicksave bindings
// need separate patches. The menu reaches CSaveGamePage through its descriptor pointer
// (0x108541A8), so removing the class name does not disable the entry.

static bool bLimitedSaving = false;

/*
  Why patching the builder is not enough on its own.

  The pause menu page is made once and kept: its factory (Dunia+7F7720) looks the class up in the
  page registry and only allocates on a miss. The entry list is built with the page, so a patch
  applied while the game is running only takes effect once the registry is thrown away, which is
  what going back to the main menu does.

  The opening does happen every time. CFCXPauseMenuPage::Open is slot 2 of the page vtable,
  Dunia+857150, so the entry list is rebuilt from there: clear the rows, build them again, and let
  the stock open run over the result.

  Nothing has to be remembered for the comparison: the builder writes the save entry's item index
  to page+0x1ECh, or -1 when the entry was skipped, which Dunia+857728 already reads. So the page
  is rebuilt only when what it was built with disagrees with the setting.
*/

// The head of CFCXPauseMenuPage::Open. Unique on the frame size and the two constants it loads; the
// string pointer in the middle is masked because it is an absolute.
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

            // All pause menu entries share this block shape. Skipping the save block leaves the
            // index at -1, which the engine already handles at 0x10857728.
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

                // Only worth installing where the patch above exists, since without it the rebuild
                // would produce the same list it just threw away.
                if (auto* pPauseOpen = dunia_find(szPauseOpenPattern))
                {
                    static auto PauseOpenHook = safetyhook::create_mid(pPauseOpen, [](SafetyHookContext& regs)
                    {
                        auto pPage = reinterpret_cast<uint8_t*>(regs.ecx);
                        if (pPage == nullptr)
                            return;

                        // The clear is the half that can decline: Dunia+CDBA50 returns without
                        // doing anything while the page's document is down, and a build on top of
                        // a clear that did not happen is the whole menu twice. A page that is not
                        // ready is left for the next opening.
                        if (*reinterpret_cast<uint8_t*>(pPage + nPageShown) == 0)
                            return;

                        // What the page was built with, in its own words.
                        auto bEntryGone = *reinterpret_cast<int32_t*>(pPage + nPauseSaveEntry) == -1;
                        if (bEntryGone == bLimitedSaving)
                            return;

                        // Ahead of the stock open, which walks the entries into the list widget
                        // once this returns. Clearing first keeps the rebuild from doubling the list.
                        auto ppVTable = *reinterpret_cast<uintptr_t**>(pPage);
                        reinterpret_cast<PageMethod_t>(ppVTable[nClearEntriesSlot])(pPage, nullptr);
                        reinterpret_cast<PageMethod_t>(ppVTable[nBuildEntriesSlot])(pPage, nullptr);
                    });
                }

                break;
            }

            /*
              F5 and F9, refused where they are acted on rather than unbound where they are read.
              Clearing the signal name as the XML binding is parsed also works, but the bindings are
              read at startup and never again, so the setting would be fixed for the run.

              Both keys arrive at the same handler, Dunia+71AF80, as a signal id compared against
              precomputed CRC-32s: 0xEFEF8B90 is crc32("quicksave") and 0x9F8F5553 is
              crc32("quickload"), which is what identifies the two branches below.

                quicksave   asks the save manager's vtable +50h whether saving is allowed at all and
                            skips out on a no. Everything past that test, the QUICK_SAVING banner
                            included, is skipped with it.
                quickload   fetches the same manager and skips out when the fetch comes back null,
                            before it reads the slot and loads it.

              So the refusal is one register in each case, read every time the key is pressed, and
              the binding itself is left exactly as the game made it.
            */
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

                        // The engine's own "no", given in its own words: the branch already knows
                        // how to be told saving is not available.
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

                        // Nothing is dereferenced on this path once the fetch reads as null. The
                        // branch falls straight to its own return.
                        regs.eax = 0;
                    });
            }
        };
    }
} LimitedSaving;
