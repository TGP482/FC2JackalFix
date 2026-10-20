module;

#include <common.hxx>
#include <intrin.h>
#include <winver.h>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <format>

#pragma comment(lib, "version.lib")

export module logging;

import common;

static HMODULE SelfModule()
{
    HMODULE hSelf = nullptr;
    const auto nFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    GetModuleHandleExW(nFlags, reinterpret_cast<LPCWSTR>(&SelfModule), &hSelf);
    return hSelf;
}

static std::filesystem::path SelfPath()
{
    return GetModulePath<std::filesystem::path>(SelfModule());
}

static std::ofstream& LogFile()
{
    static std::ofstream file(std::filesystem::path(SelfPath()).replace_extension(".log"), std::ios::trunc);
    return file;
}

export void LogWrite(std::string_view level, std::string_view text)
{
    static std::mutex mutex;
    std::scoped_lock lock(mutex);

    auto& file = LogFile();
    if (!file)
        return;

    SYSTEMTIME now{};
    GetLocalTime(&now);

    file << std::format("[{:02}:{:02}:{:02}.{:03}] [{}] {}\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        level, text);
    file.flush();
}

export template <class... Args>
void LogInfo(std::format_string<Args...> format, Args&&... args)
{
    LogWrite("info", std::format(format, std::forward<Args>(args)...));
}

export template <class... Args>
void LogWarn(std::format_string<Args...> format, Args&&... args)
{
    LogWrite("warn", std::format(format, std::forward<Args>(args)...));
}

static std::string Trim(std::string text)
{
    const auto nFirst = text.find_first_not_of(' ');
    const auto nLast = text.find_last_not_of(' ');
    return nFirst == std::string::npos ? std::string() : text.substr(nFirst, nLast - nFirst + 1);
}

static std::string RegString(HKEY hKey, const char* szName)
{
    char szBuffer[256]{};
    DWORD nSize = sizeof(szBuffer) - 1;

    auto pBuffer = reinterpret_cast<LPBYTE>(szBuffer);
    if (RegQueryValueExA(hKey, szName, nullptr, nullptr, pBuffer, &nSize) != ERROR_SUCCESS)
        return {};

    return szBuffer;
}

static std::string FileVersion(const std::filesystem::path& path)
{
    DWORD nIgnored = 0;
    const auto nSize = GetFileVersionInfoSizeW(path.c_str(), &nIgnored);
    if (!nSize)
        return {};

    std::vector<uint8_t> data(nSize);
    UINT nLength = 0;

    if (!GetFileVersionInfoW(path.c_str(), 0, nSize, data.data()))
        return {};

    // The string block holds the version a game is patched to; the fixed block is often the build default.
    struct Translation { WORD wLanguage; WORD wCodePage; };
    Translation* pTranslation = nullptr;
    auto ppTranslation = reinterpret_cast<void**>(&pTranslation);

    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", ppTranslation, &nLength)
        && nLength >= sizeof(Translation))
    {
        const auto szQuery = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\FileVersion", pTranslation->wLanguage,
            pTranslation->wCodePage);

        wchar_t* szValue = nullptr;
        auto bFound = VerQueryValueW(data.data(), szQuery.c_str(), reinterpret_cast<void**>(&szValue), &nLength);
        if (bFound && szValue && *szValue)
            return Trim(std::string(szValue, szValue + wcslen(szValue)));
    }

    VS_FIXEDFILEINFO* pFixed = nullptr;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&pFixed), &nLength) || !pFixed)
        return {};

    return std::format("{}.{}.{}.{}", HIWORD(pFixed->dwFileVersionMS), LOWORD(pFixed->dwFileVersionMS),
        HIWORD(pFixed->dwFileVersionLS), LOWORD(pFixed->dwFileVersionLS));
}

static constexpr auto szCurrentVersionKey = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
static constexpr REGSAM nCurrentVersionAccess = KEY_READ | KEY_WOW64_64KEY;

static std::string OSVersion()
{
    std::string szName;
    std::string szDisplay;
    std::string szBuild;
    DWORD nUbr = 0;

    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, szCurrentVersionKey, 0, nCurrentVersionAccess, &hKey) == ERROR_SUCCESS)
    {
        szName = RegString(hKey, "ProductName");
        szDisplay = RegString(hKey, "DisplayVersion");
        szBuild = RegString(hKey, "CurrentBuildNumber");

        DWORD nSize = sizeof(nUbr);
        RegQueryValueExA(hKey, "UBR", nullptr, nullptr, reinterpret_cast<LPBYTE>(&nUbr), &nSize);
        RegCloseKey(hKey);
    }

    if (szName.empty())
        return "unknown";

    // ProductName still reads "Windows 10" on 11; the build number is the only tell.
    if (std::atoi(szBuild.c_str()) >= 22000)
    {
        const auto nTen = szName.find("Windows 10");
        if (nTen != std::string::npos)
            szName.replace(nTen, 10, "Windows 11");
    }

    if (!szDisplay.empty())
        szName += " " + szDisplay;

    if (!szBuild.empty())
        szName += std::format(" (build {}.{})", szBuild, nUbr);

    return szName;
}

// A compatibility mode lies to RtlGetVersion, which is why the build above comes from the registry.
static void LogShimmedVersion()
{
    using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info{ sizeof(info) };
    const auto pRtlGetVersion =
        reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));

    if (!pRtlGetVersion || pRtlGetVersion(&info) != 0)
        return;

    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, szCurrentVersionKey, 0, nCurrentVersionAccess, &hKey) != ERROR_SUCCESS)
        return;

    const auto nReal = std::atoi(RegString(hKey, "CurrentBuildNumber").c_str());
    RegCloseKey(hKey);

    if (nReal && info.dwBuildNumber != static_cast<DWORD>(nReal))
    {
        LogWarn("OS: Compatibility mode is on, the game sees {}.{} build {}", info.dwMajorVersion, info.dwMinorVersion,
            info.dwBuildNumber);
    }
}

static std::string HostVersion()
{
    const auto hNtdll = GetModuleHandleW(L"ntdll.dll");
    using WineVersion_t = const char* (__cdecl*)();
    using WineHost_t = void(__cdecl*)(const char**, const char**);

    const auto pWineVersion = reinterpret_cast<WineVersion_t>(GetProcAddress(hNtdll, "wine_get_version"));
    if (!pWineVersion)
        return {};

    auto szHost = std::format("Wine {}", pWineVersion());

    const auto pWineHost = reinterpret_cast<WineHost_t>(GetProcAddress(hNtdll, "wine_get_host_version"));
    const char* szSysname = nullptr;
    const char* szRelease = nullptr;

    if (pWineHost)
    {
        pWineHost(&szSysname, &szRelease);
        if (szSysname && szRelease)
            szHost += std::format(" on {} {}", szSysname, szRelease);
    }

    return szHost;
}

static std::string CPUName()
{
    int regs[4]{};
    __cpuid(regs, 0x80000000);

    if (static_cast<uint32_t>(regs[0]) < 0x80000004)
        return "unknown";

    char szBrand[49]{};
    for (int i = 0; i < 3; i++)
    {
        __cpuid(regs, 0x80000002 + i);
        memcpy(szBrand + i * sizeof(regs), regs, sizeof(regs));
    }

    return Trim(szBrand);
}

static void LogAdapters()
{
    std::vector<std::string> seen;

    for (DWORD i = 0;; i++)
    {
        DISPLAY_DEVICEA device{ sizeof(device) };
        if (!EnumDisplayDevicesA(nullptr, i, &device, 0))
            break;

        // Every monitor reports its adapter, so each adapter is listed once.
        if (std::find(seen.begin(), seen.end(), device.DeviceString) != seen.end())
            continue;

        seen.emplace_back(device.DeviceString);
        const auto szPrimary = (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "";
        LogInfo("GPU {}: {}{}", seen.size(), device.DeviceString, szPrimary);
    }

    if (seen.empty())
        LogWarn("GPU: EnumDisplayDevices returned nothing");
}

static void LogSystemInfo()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    LogInfo("Starting FC2JackalFix {} ({}) at {:04}-{:02}-{:02} {:02}:{:02}:{:02}", rsc_Version, rsc_FileVersion,
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    const auto asiPath = SelfPath();
    const auto exeDir = GetExeModulePath<std::filesystem::path>();
    const auto szGameVersion = FileVersion(GetModulePath<std::filesystem::path>(nullptr));

    const auto nAsiBase = reinterpret_cast<uintptr_t>(SelfModule());
    const auto nGameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto szGameName = GetExeModuleName<std::filesystem::path>().string();
    const auto szGameSuffix = szGameVersion.empty() ? std::string() : " " + szGameVersion;

    LogInfo("ASI: {} (base 0x{:08X})", asiPath.lexically_proximate(exeDir).string(), nAsiBase);
    LogInfo("Game: {}{} (base 0x{:08X})", szGameName, szGameSuffix, nGameBase);
    LogInfo("Path: {}", exeDir.string());

    BOOL bWow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &bWow64);

    const auto szHost = HostVersion();
    const auto szHostSuffix = szHost.empty() ? std::string() : " - " + szHost;
    LogInfo("OS: {} {}{}", OSVersion(), bWow64 ? "64-bit" : "32-bit", szHostSuffix);
    LogShimmedVersion();

    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    LogInfo("CPU: {} ({} threads)", CPUName(), system.dwNumberOfProcessors);

    MEMORYSTATUSEX memory{ sizeof(memory) };
    GlobalMemoryStatusEx(&memory);
    LogInfo("RAM: {:.1f} GB", memory.ullTotalPhys / 1073741824.0);

    LogAdapters();

    // The path CIniReader("") resolves to.
    LogInfo("Config: {}", std::filesystem::path(asiPath).replace_extension(".ini").string());
}

// Ahead of every other init handler, so the header is at the top of the file.
static constexpr auto nInitPriority = 10;

class Logging
{
public:
    Logging()
    {
        JackalFix::onInitEvent().add([]()
        {
            LogSystemInfo();
        }, nInitPriority);
    }
} Logging;
