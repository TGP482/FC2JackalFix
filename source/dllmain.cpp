#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mutex>
#include <functional>
#include <string_view>

import common;
import dunia;
import settings;
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
import vibration;
import controllerprompts;
import loadingscreen;
import systemdetection;
import maxfps;
import aniso;
import renderconfig;
import rimlighting;
import saturation;
import borderless;
import internalres;
import affinity;
import utilisation;
import largeaddressaware;
import limitedsaving;
import saveonmissioncomplete;
import blinkingitems;
import hitindicator;
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
import logging;
import effectsorting;
import jackalfixmenu;
import updatecheck;

void Init()
{
    JackalFixSettings.ReadIniSettings();
    JackalFix::onStartupPromptEvent().executeAll();
    JackalFix::onInitEvent().executeAll();
}

extern "C"
{
    void __declspec(dllexport) InitializeASI()
    {
        std::call_once(CallbackHandler::flag, []()
        {
            CallbackHandler::RegisterCallback(Init);
            CallbackHandler::RegisterCallback(L"Dunia.dll", InitDunia);
        });
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // UAL calls InitializeASI itself; other loaders do not.
        if (!IsUALPresent()) { InitializeASI(); }
    }
    if (reason == DLL_PROCESS_DETACH)
    {
        JackalFix::onShutdownEvent().executeAll();
    }
    return TRUE;
}
