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
            // Splash screens share CFCXSplashPage::Update. Skipping this branch follows the game's existing
            // SkipIntroMovies path without affecting cutscenes or credits.
            auto pattern = dunia_pattern("80 BE 64 01 00 00 00 75 1B A1 ? ? ? ? 83 B8 90 00 00 00 00 76 11");
            if (pattern.empty())
                return;

            static raw_mem fnSkipIntro(pattern.get_first(21), { 0x90, 0x90 });

            static auto SkipIntroCB = []()
            {
                if (JackalFixSettings.GetInt(PREF_SKIPINTRO))
                    fnSkipIntro.Write();
                else
                    fnSkipIntro.Restore();
            };

            SkipIntroCB();

            JackalFix::onIniFileChange() += []()
            {
                SkipIntroCB();
            };
        };
    }
} SkipIntro;
