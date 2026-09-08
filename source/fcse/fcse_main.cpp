// Entry point for the FCSE build, in place of dllmain.cpp's ASI one.
//
// FCSE loads plugins once Dunia.dll is mapped and before any engine code runs, which is both of the
// moments the ASI build waits for separately, so the two init events fire back to back here.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <fcse_api.h>

import common;
import dunia;
import settings;
import logging;

import skipintro;
import titlescreen;
import fov;
import jackaltapes;
import bonuscontent;
import menucursor;
import mousespeed;
import inputdevice;
import looksensitivity;
import inputtoggles;
import turretexitbind;
import custombinds;
import vibration;
import controllerprompts;
import loadingscreen;
import systemdetection;
import maxfps;
import aniso;
import renderconfig;
import rimlighting;
import postfx;
import saturation;
import borderless;
import internalres;
import affinity;
import utilisation;
import limitedsaving;
import saveonmissioncomplete;
import blinkingitems;
import hitindicator;
import hudfadeout;
import tutorialmessages;
import glider;
import truck;
import dlcvehiclecolors;
import entitylibrary;
import glassdecals;
import fpsfixes;
import guiduplicates;
import hudfixes;
import debug;
import effectsorting;

extern "C"
{
    bool __declspec(dllexport) FCSE_Load(const FCSE_PluginAPI* pApi)
    {
        if (pApi == nullptr)
            return false;

        if (pApi->apiVersion != FCSE_API_VERSION)
        {
            pApi->Log("FC2JackalFix was built against a different FCSE plugin API, not loading");
            return false;
        }

        if (!FCSE::Bind(pApi))
        {
            pApi->Log("FC2JackalFix could not bind the FCSE address library, not loading");
            return false;
        }

        // What InitDunia() works out for itself in the ASI build. Calling it instead would fire the
        // Dunia event a second time, once per handler already run below.
        hDunia = pApi->duniaModule;
        bDuniaGOG = pApi->gameBuild == FCSE_GAME_BUILD_103_RETAIL;

        if (hDunia == nullptr)
        {
            pApi->Log("FC2JackalFix was handed no Dunia.dll, not loading");
            return false;
        }

        // Before the events: every Bind applies its setting as it subscribes, so the values have to
        // be in hand first.
        RegisterJackalFixSettings();

        JackalFix::onInitEvent().executeAll();

        LogInfo("Dunia: {} build via FCSE (base 0x{:08X})", bDuniaGOG ? "GOG" : "Steam",
                reinterpret_cast<uintptr_t>(hDunia));

        JackalFix::onDuniaInitEvent().executeAll();

        pApi->Log("FC2JackalFix loaded");
        return true;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_DETACH)
    {
        JackalFix::onShutdownEvent().executeAll();
    }
    return TRUE;
}
