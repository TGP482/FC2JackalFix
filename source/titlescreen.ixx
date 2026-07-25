module;

#include <common.hxx>

export module titlescreen;

import common;
import dunia;
import settings;

class SkipTitleScreen
{
public:
    SkipTitleScreen()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
			// MAINMENU_INITIAL_PRESENTATION waits for input before transitioning to CFCXMainPage.
			// Taking the existing branch immediately skips the prompt without forcing a page change.
            auto pattern = dunia_pattern("8B 86 6C 01 00 00 83 E8 01 74 ? 8B 86 68 01 00 00");
            if (pattern.empty())
                return;

            static raw_mem fnSkipTitleScreen(pattern.get_first(8), { 0x00 });

            static auto SkipTitleScreenCB = []()
            {
                if (JackalFixSettings.GetInt(PREF_SKIPTITLESCREEN))
                    fnSkipTitleScreen.Write();
                else
                    fnSkipTitleScreen.Restore();
            };

            SkipTitleScreenCB();

            JackalFix::onIniFileChange() += []()
            {
                SkipTitleScreenCB();
            };
        };
    }
} SkipTitleScreen;
