module;

#include <common.hxx>
#include <winhttp.h>
#include <shellapi.h>
#include <commctrl.h>
#include <regex>
#include <fstream>
#include <ctime>
#include <cctype>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

export module updatecheck;

import common;

static constexpr auto szTitle = L"FC2JackalFix";

// Same address rsc_UpdateUrl puts in the version resource.
static constexpr auto szUpdateUrl = L"https://github.com/TGP482/FC2JackalFix";
static constexpr auto szApiHost = L"api.github.com";
static constexpr auto szApiPath = L"/repos/TGP482/FC2JackalFix/releases/latest";

// Answers live beside the asi, not in the ini.
static constexpr auto szCacheName = L"FC2JackalFix.update";
static constexpr auto nCacheTTLSeconds = 24 * 60 * 60;

// RT_MANIFEST id in Versioninfo.rc. 2 not 1: activated by this asi for itself, not by the host.
static constexpr auto nManifestResourceId = 2;

// CI passes --with-version, so field one is the release number. A local build gets day.month.year,
// which reads ahead of any release and never prompts.
static constexpr auto szInstalledVersion = rsc_FileVersion;

static std::wstring Widen(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

// Releases are whole numbers: 1.2.3, v2, V2.0 are V1, V2, V2.
static int ParseVersion(const std::string& version)
{
    size_t start = 0;
    while (start < version.size() && !std::isdigit(static_cast<unsigned char>(version[start])))
        start++;

    if (start >= version.size())
        return 0;

    try
    {
        return std::stoi(version.substr(start));
    }
    catch (...)
    {
        return 0;
    }
}

static std::wstring FormatVersion(const std::string& version)
{
    return L"V" + std::to_wstring(ParseVersion(version));
}

static int CompareVersion(const std::string& left, const std::string& right)
{
    const auto nLeft = ParseVersion(left);
    const auto nRight = ParseVersion(right);

    if (nLeft == nRight)
        return 0;

    return nLeft < nRight ? -1 : 1;
}

static std::filesystem::path GetCachePath()
{
    return GetThisModulePath<std::filesystem::path>() / szCacheName;
}

// Five lines: latest release, fetch time, release already prompted for, installed version (expires
// the cache on upgrade), release silenced. A pre-tick-box cache has no fifth line.
static bool LoadCache(std::string& latest, std::string& mentioned, bool& bFresh, std::string& silenced)
{
    std::ifstream file(GetCachePath());
    if (!file)
        return false;

    std::string when;
    std::string installed;

    if (!std::getline(file, latest) || !std::getline(file, when))
        return false;

    if (!std::getline(file, mentioned) || !std::getline(file, installed))
        return false;

    std::getline(file, silenced);

    auto nWhen = 0ll;
    try
    {
        nWhen = std::stoll(when);
    }
    catch (...)
    {
        nWhen = 0;
    }

    const auto nAge = static_cast<long long>(std::time(nullptr)) - nWhen;
    bFresh = nAge >= 0 && nAge < nCacheTTLSeconds && installed == szInstalledVersion;

    return !latest.empty();
}

static void SaveCache(const std::string& latest, const std::string& mentioned, const std::string& silenced)
{
    std::ofstream file(GetCachePath(), std::ios::trunc);
    if (!file)
        return;

    file << latest << "\n";
    file << static_cast<long long>(std::time(nullptr)) << "\n";
    file << mentioned << "\n";
    file << szInstalledVersion << "\n";
    file << silenced << "\n";
}

static bool IsSuccessStatus(HINTERNET hRequest)
{
    DWORD nStatus = 0;
    DWORD nStatusSize = sizeof(nStatus);
    const auto nQuery = WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER;

    const auto bQueried = WinHttpQueryHeaders(hRequest, nQuery, WINHTTP_HEADER_NAME_BY_INDEX, &nStatus, &nStatusSize,
        WINHTTP_NO_HEADER_INDEX);

    return bQueried && nStatus >= 200 && nStatus < 300;
}

static void ReadBody(HINTERNET hRequest, std::string& response)
{
    DWORD nAvailable = 0;
    do
    {
        if (!WinHttpQueryDataAvailable(hRequest, &nAvailable) || nAvailable == 0)
            break;

        std::string chunk(nAvailable, '\0');
        DWORD nRead = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), nAvailable, &nRead))
            break;

        response.append(chunk, 0, nRead);

        // tag_name is near the front; the release body can run long.
        if (response.size() > 256 * 1024)
            break;
    } while (nAvailable > 0);
}

// Every failure ends the check silently. 5 s on all four timeouts, so a firewalled machine does not
// stall. A repo with no releases answers 404 and takes the same path.
static std::string FetchLatestRelease()
{
    std::string response;
    const auto userAgent = std::wstring(L"FC2JackalFix/") + Widen(szInstalledVersion);

    auto hSession = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return response;

    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    auto hConnect = WinHttpConnect(hSession, szApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return response;
    }

    auto hRequest = WinHttpOpenRequest(hConnect, L"GET", szApiPath, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return response;
    }

    // GitHub rejects a request with no user agent; the media type pins the reply shape.
    const auto szAccept = L"Accept: application/vnd.github+json";
    WinHttpAddRequestHeaders(hRequest, szAccept, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

    auto bSent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bSent && WinHttpReceiveResponse(hRequest, nullptr) && IsSuccessStatus(hRequest))
        ReadBody(hRequest, response);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

static bool QueryLatestVersion(std::string& latest)
{
    const auto response = FetchLatestRelease();
    if (response.empty())
        return false;

    // One field, so a regex rather than a json dependency. Tags are v1.2.3 and the v is not part of
    // the version. The custom delimiter is because the pattern's own )" would close a plain R"( ).
    std::smatch match;
    const std::regex tag(R"rx("tag_name"\s*:\s*"\s*[vV]?([0-9][^"]*)")rx");

    if (!std::regex_search(response, match, tag) || match.size() < 2)
        return false;

    latest = match[1];
    return true;
}

// TaskDialogIndirect is the only prompt with a tick box, and only comctl32 v6 exports it (system32
// holds 5.82). Bound via an activation context on this asi's RT_MANIFEST, held across load and call.
class ComCtl6Scope
{
public:
    ComCtl6Scope()
    {
        HMODULE hSelf = nullptr;
        const auto nFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!GetModuleHandleExW(nFlags, reinterpret_cast<LPCWSTR>(&szTitle), &hSelf) || !hSelf)
            return;

        ACTCTXW context{};
        context.cbSize = sizeof(context);
        context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        context.hModule = hSelf;
        context.lpResourceName = MAKEINTRESOURCEW(nManifestResourceId);

        mContext = CreateActCtxW(&context);
        if (mContext == INVALID_HANDLE_VALUE)
            return;

        mActivated = ActivateActCtx(mContext, &mCookie) != FALSE;
    }

    ~ComCtl6Scope()
    {
        if (mActivated)
            DeactivateActCtx(0, mCookie);

        if (mContext != INVALID_HANDLE_VALUE)
            ReleaseActCtx(mContext);
    }

    bool IsActive() const { return mActivated; }

private:
    HANDLE mContext = INVALID_HANDLE_VALUE;
    ULONG_PTR mCookie = 0;
    bool mActivated = false;
};

struct UpdatePrompt
{
    std::wstring instruction;
    std::wstring content;
    std::wstring verification;
};

// Fails when the activation context or the export is missing.
static HRESULT ShowTaskDialog(const UpdatePrompt& prompt, int& nButton, BOOL& bChecked)
{
    ComCtl6Scope comctl6;
    if (!comctl6.IsActive())
        return E_FAIL;

    auto hComCtl = LoadLibraryW(L"comctl32.dll");
    if (!hComCtl)
        return E_FAIL;

    using TaskDialogIndirect_t = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    auto pTaskDialogIndirect = reinterpret_cast<TaskDialogIndirect_t>(GetProcAddress(hComCtl, "TaskDialogIndirect"));

    auto hr = E_FAIL;
    if (pTaskDialogIndirect)
    {
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        config.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
        config.pszWindowTitle = szTitle;
        config.pszMainIcon = TD_INFORMATION_ICON;
        config.pszMainInstruction = prompt.instruction.c_str();
        config.pszContent = prompt.content.c_str();
        config.pszVerificationText = prompt.verification.c_str();

        hr = pTaskDialogIndirect(&config, &nButton, nullptr, &bChecked);
    }

    FreeLibrary(hComCtl);
    return hr;
}

// Yes opens the page, No closes, and the tick box silences this release under either. Without the
// task dialog it falls back to MB_YESNOCANCEL, Cancel standing in for the tick box.
static bool AskAboutUpdate(const UpdatePrompt& prompt, bool& bSilence)
{
    auto nButton = 0;
    auto bChecked = FALSE;
    if (SUCCEEDED(ShowTaskDialog(prompt, nButton, bChecked)))
    {
        bSilence = bChecked != FALSE;
        return nButton == IDYES;
    }

    const auto text = prompt.instruction + L"\n\n" + prompt.content + L"\n\nCancel is the tick box: "
        + prompt.verification + L".";
    const auto nFlags = MB_YESNOCANCEL | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND;
    const auto nAnswer = MessageBoxW(nullptr, text.c_str(), szTitle, nFlags);

    bSilence = nAnswer == IDCANCEL;
    return nAnswer == IDYES;
}

static void ShowUpdateMessage(const std::string& installed, const std::string& latest, bool& bSilence)
{
    UpdatePrompt prompt;
    prompt.instruction = L"A new version of FC2JackalFix is available.";
    prompt.content = std::wstring(L"Installed: ") + FormatVersion(installed) + L"\n"
        + L"Latest: " + FormatVersion(latest) + L"\n\n"
        + L"Releases are published on GitHub at\n" + szUpdateUrl + L"\n\n"
        + L"Open the download page now?";
    prompt.verification = L"Don't show again";

    bSilence = false;
    if (!AskAboutUpdate(prompt, bSilence))
        return;

    ShellExecuteW(nullptr, L"open", szUpdateUrl, nullptr, nullptr, SW_SHOWNORMAL);
}

static void CheckForUpdates()
{
    std::string latest;
    std::string mentioned;
    std::string silenced;
    auto bFresh = false;

    const auto bHadCache = LoadCache(latest, mentioned, bFresh, silenced);

    if (!bHadCache || !bFresh)
    {
        std::string fetched;
        if (!QueryLatestVersion(fetched))
            return;

        // A different release than the cached one has not been prompted for.
        if (CompareVersion(fetched, latest) != 0)
            mentioned.clear();

        latest = fetched;
        SaveCache(latest, mentioned, silenced);
    }

    // Installed ahead of the release is a local build; equal is up to date.
    if (CompareVersion(szInstalledVersion, latest) >= 0)
        return;

    // The tick box covers its own release and nothing later: V3 asks again after V2 was silenced.
    if (!silenced.empty() && CompareVersion(latest, silenced) <= 0)
        return;

    if (!mentioned.empty() && CompareVersion(mentioned, latest) == 0)
        return;

    // Recorded before the prompt, so a crash while it is open does not repeat it next launch.
    SaveCache(latest, latest, silenced);

    auto bSilence = false;
    ShowUpdateMessage(szInstalledVersion, latest, bSilence);

    if (bSilence)
        SaveCache(latest, latest, latest);
}

// Main thread, so the game waits for the answer. First startup prompt; skipped on a fresh cache.
static constexpr auto nStartupPromptPriority = 10;

class UpdateCheck
{
public:
    UpdateCheck()
    {
        JackalFix::onStartupPromptEvent().add([]()
        {
            CheckForUpdates();
        }, nStartupPromptPriority);
    }
} UpdateCheck;
