module;

#include <common.hxx>

export module skipintro;

import common;
import dunia;
import settings;

class SkipIntro
{
public:
    SkipIntro()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Splash screens share CFCXSplashPage::Update. Skipping this branch follows the
            // game's existing SkipIntroMovies path without affecting cutscenes or credits.
            auto* pSkipIntro = dunia_find("80 BE 64 01 00 00 00 75 1B A1 ? ? ? ? 83 B8 90 00 00 00 00 76 11", 21);
            if (!pSkipIntro)
                return;

            static raw_mem fnSkipIntro(pSkipIntro, { 0x90, 0x90 });
            BindPatch(fnSkipIntro, PREF_SKIPINTRO);
        };
    }
} SkipIntro;
