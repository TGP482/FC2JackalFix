/*
  One log file for the parts of the mod whose work is invisible until it goes wrong.

  Everything here is a pattern lifted out of Dunia and a pointer walked from it, and when one of
  those fails it fails silently: a null pointer means a feature quietly does nothing, and from the
  outside that is indistinguishable from the feature being wrong. Guessing which of the two it was
  costs a build and a play session each time. This costs one line per event.

  Started fresh once per run, so a log is the story of one session rather than of every session since
  the plugin was installed. Written whole lines at a time through an append handle, so two threads
  logging at once interleave lines rather than characters.
*/

module;

#include <common.hxx>
#include <cstdarg>
#include <cstdio>

export module jflog;

import common;

export void JFTrace(const char* pFormat, ...)
{
    static auto sPath = GetThisModulePath<std::string>() + "FC2JackalFix.log";

    static bool bTruncated = []()
    {
        auto hNew = CreateFileA(sPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hNew != INVALID_HANDLE_VALUE)
            CloseHandle(hNew);
        return true;
    }();
    (void)bTruncated;

    char szLine[512]{};
    va_list args;
    va_start(args, pFormat);
    auto nLength = vsnprintf(szLine, sizeof(szLine) - 3, pFormat, args);
    va_end(args);
    if (nLength < 0)
        return;

    szLine[nLength++] = '\r';
    szLine[nLength++] = '\n';

    auto hFile = CreateFileA(sPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    DWORD nWritten = 0;
    WriteFile(hFile, szLine, static_cast<DWORD>(nLength), &nWritten, nullptr);
    CloseHandle(hFile);
}
