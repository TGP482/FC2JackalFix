module;

#include <common.hxx>

export module controller;

import common;
import dunia;
import settings;

// Gamepad aim assist is handled by CActionMapPadFilter.
// All four helpers are applied from one function, so gating here disables them all.
static bool bAimAssist = true;

static constexpr size_t nAimAssistSites = 4;

// Each helper block is 0x1E bytes. These are offsets from the CMP to the block and its end.
static constexpr ptrdiff_t nHelperBlockStart = 9;
static constexpr ptrdiff_t nHelperBlockEnd = 0x1E;

// One pattern per site. The blocks differ in scratch register, so a single wildcarded
// pattern is not reliable.
static constexpr std::array<std::string_view, nAimAssistSites> sAimAssistPatterns =
{
    "80 BE 5D 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 4C 24 24 51 8B CE E8 ? ? ? ?", // stickyHelper
    "80 BE 5C 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 54 24 24 52 8B CE E8 ? ? ? ?", // followEnemyHelper
    "80 BE 5F 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 44 24 24 50 8B CE E8 ? ? ? ?", // ShootCorrectionHelper
    "80 BE 5E 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 4C 24 24 51 8B CE E8 ? ? ? ?", // IronSightHelper
};

static uintptr_t nHelperJoin[nAimAssistSites] = {};

// Skip the helper rather than clear its enable flag, which is reused elsewhere.
template <size_t nSite>
static void SkipAimAssistHelper(SafetyHookContext& regs)
{
    if (!bAimAssist)
        regs.eip = nHelperJoin[nSite];
}

// Hooked independently so one pattern failure does not lose the other three.
template <size_t nSite>
static void InstallAimAssistSkip()
{
    auto pattern = dunia_pattern(sAimAssistPatterns[nSite]);
    if (pattern.empty())
        return;

    nHelperJoin[nSite] = reinterpret_cast<uintptr_t>(pattern.get_first(nHelperBlockEnd));

    static auto AimAssistHook = safetyhook::create_mid(pattern.get_first(nHelperBlockStart), SkipAimAssistHelper<nSite>);
}

class Controller
{
public:
    Controller()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // 107DA91A  CMP   byte ptr [ESI+0x15E],0
            // 107DA921  JZ    0x107DA938
            // 107DA923  LEA   ECX,[ESP+0x24]           <- hook
            // 107DA927  CALL  0x107D9020
            // 107DA92C  LEA   ECX,[ESP+0x24]           ; edx at one site, eax at another
            // 107DA930  PUSH  ECX
            // 107DA931  MOV   ECX,ESI
            // 107DA933  CALL  0x107DA2F0               ; the helper
            // 107DA938  ...                            <- join
            InstallAimAssistSkip<0>();
            InstallAimAssistSkip<1>();
            InstallAimAssistSkip<2>();
            InstallAimAssistSkip<3>();

            static auto ControllerCB = []()
            {
                bAimAssist = JackalFixSettings.GetInt(PREF_AIMASSIST) != 0;
            };

            ControllerCB();

            JackalFix::onIniFileChange() += []()
            {
                ControllerCB();
            };
        };
    }
} Controller;
