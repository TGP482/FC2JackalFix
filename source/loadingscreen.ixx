module;

#include <common.hxx>
#include <timeapi.h>

export module loadingscreen;

import common;
import settings;

// Timer resolution restores loading screen frame pacing.
static bool bTimerRaised = false;

class LoadingScreen
{
public:
    LoadingScreen()
    {
        JackalFix::onInitEvent() += []()
        {
            ApplyAndWatch([]()
            {
                auto wanted = JackalFixSettings.GetInt(PREF_HIGHPRECISIONTIMER) != 0;
                if (wanted == bTimerRaised)
                    return;

                if (wanted)
                    timeBeginPeriod(1);
                else
                    timeEndPeriod(1);

                bTimerRaised = wanted;
            });
        };

        JackalFix::onShutdownEvent() += []()
        {
            if (bTimerRaised)
                timeEndPeriod(1);
        };
    }
} LoadingScreen;
