module;

#include <common.hxx>

export module affinity;

import common;
import settings;

static bool bMaskApplied = false;
static DWORD_PTR nOriginalMask = 0;

static void ApplyAffinity()
{
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;

    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        return;

    if (!bMaskApplied)
        nOriginalMask = processMask;

    const auto nProcessors = JackalFixSettings.GetInt(PREF_CPUAFFINITY);

    if (nProcessors <= 0)
    {
        // Clearing the ini key mid-run restores the processors.
        if (bMaskApplied)
        {
            SetProcessAffinityMask(GetCurrentProcess(), nOriginalMask);
            bMaskApplied = false;
        }
        return;
    }

    // From the system mask; processor numbers may be sparse
    DWORD_PTR mask = 0;
    int32_t taken = 0;

    for (DWORD_PTR bit = 1; bit != 0 && taken < nProcessors; bit <<= 1)
    {
        if (systemMask & bit)
        {
            mask |= bit;
            ++taken;
        }
    }

    if (mask == 0)
        return;

    if (SetProcessAffinityMask(GetCurrentProcess(), mask))
        bMaskApplied = true;
}

class CpuAffinity
{
public:
    CpuAffinity()
    {
        JackalFix::onInitEvent() += []()
        {
            // Before Dunia init, which sizes its thread pools from the process affinity mask.
            // Affinity itself is live; thread counts need a restart.
            ApplyAndWatch(ApplyAffinity);
        };

        JackalFix::onShutdownEvent() += []()
        {
            if (bMaskApplied)
                SetProcessAffinityMask(GetCurrentProcess(), nOriginalMask);
        };
    }
} CpuAffinity;
