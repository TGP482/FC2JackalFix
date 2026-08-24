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
            // MAINMENU_INITIAL_PRESENTATION waits for input before CFCXMainPage.
            auto* pSkipTitleScreen = dunia_find("8B 86 6C 01 00 00 83 E8 01 74 ? 8B 86 68 01 00 00", 8);
            if (!pSkipTitleScreen)
                return;

            static raw_mem fnSkipTitleScreen(pSkipTitleScreen, { 0x00 });
            BindPatch(fnSkipTitleScreen, PREF_SKIPTITLESCREEN);
        };
    }
} SkipTitleScreen;
