module;

#include <common.hxx>
#include <timeapi.h>

export module loadingscreen;

import common;
import settings;

// Restore loading screen frame pacing by increasing timer resolution.
static bool bTimerRaised = false;

class LoadingScreen
{
public:
    LoadingScreen()
    {
        JackalFix::onInitEvent() += []()
        {
            static auto TimerCB = []()
            {
                auto wanted = JackalFixSettings.GetInt(PREF_HIGHPRECISIONTIMER) != 0;
                if (wanted == bTimerRaised)
                    return;

                if (wanted)
                    timeBeginPeriod(1);
                else
                    timeEndPeriod(1);

                bTimerRaised = wanted;
            };

            TimerCB();

            JackalFix::onIniFileChange() += []()
            {
                TimerCB();
            };
        };

        JackalFix::onShutdownEvent() += []()
        {
            if (bTimerRaised)
                timeEndPeriod(1);
        };
    }
} LoadingScreen;
