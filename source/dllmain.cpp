#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mutex>
#include <functional>
#include <string_view>


import common;
import dunia;
import settings;
import skipintro;
import fov;
import jackaltapes;
import bonuscontent;
import menucursor;
import mousespeed;
import loadingscreen;
import systemdetection;
import maxfps;
import aniso;
import x360gamma;
import borderless;
import affinity;
import limitedsaving;
import blinkingitems;

void Init()
{
    JackalFixSettings.ReadIniSettings();
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
        // UAL calls InitializeASI itself; under any other loader we have to kick it off here.
        if (!IsUALPresent()) { InitializeASI(); }
    }
    if (reason == DLL_PROCESS_DETACH)
    {
        JackalFix::onShutdownEvent().executeAll();
    }
    return TRUE;
}
