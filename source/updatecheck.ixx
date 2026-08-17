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

// Reads the newest release tag off the GitHub API and offers the download page once per release.
// Answers are kept in FC2JackalFix.update beside the asi rather than in the ini, so the prompt owns
// its own state and there is no setting to explain.
static constexpr auto szTitle = L"FC2JackalFix";

// Same address rsc_UpdateUrl puts in the version resource, read through the API.
static constexpr auto szUpdateUrl = L"https://github.com/TGP482/FC2JackalFix";
static constexpr auto szApiHost = L"api.github.com";
static constexpr auto szApiPath = L"/repos/TGP482/FC2JackalFix/releases/latest";

static constexpr auto szCacheName = L"FC2JackalFix.update";
static constexpr auto nCacheTTLSeconds = 24 * 60 * 60;

// Id of the RT_MANIFEST in Versioninfo.rc. 2 rather than 1: a dependency this asi activates for
// itself, not a manifest for the process it loads into.
static constexpr auto nManifestResourceId = 2;

// CI passes the released version to premake as --with-version, so the first field is the release
// number. A local build gets day.month.year instead, whose first field is the day, which reads far
// ahead of any release and so never prompts.
static constexpr auto szInstalledVersion = rsc_FileVersion;

static std::wstring Widen(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

// Releases are whole numbers. 1.2.3, v2 and V2.0 are V1, V2, V2.
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

// Five lines: latest release, when it was fetched, the release already prompted for, the version
// installed at the time, the release the tick box was ticked for. The installed line expires the
// cache when the fix itself is updated, so a fresh build is compared rather than trusted. A cache
// written before the tick box existed has no fifth line and reads as nothing silenced.
static bool LoadCache(std::string& latest, std::string& mentioned, bool& bFresh, std::string& silenced)
{
    std::ifstream file(GetCachePath());
    if (!file)
        return false;

    std::string when;
    std::string installed;

    if (!std::getline(file, latest) || !std::getline(file, when) || !std::getline(file, mentioned) || !std::getline(file, installed))
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

// Every failure here ends the check silently. A firewalled machine gets no dialog and no stall,
// hence 5s on all four timeouts. No releases yet answers 404, which is the same path.
static bool QueryLatestVersion(std::string& latest)
{
    const auto userAgent = std::wstring(L"FC2JackalFix/") + Widen(szInstalledVersion);

    auto hSession = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return false;

    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    auto hConnect = WinHttpConnect(hSession, szApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return false;
    }

    auto hRequest = WinHttpOpenRequest(hConnect, L"GET", szApiPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // GitHub rejects a request with no user agent. The media type pins the reply shape.
    WinHttpAddRequestHeaders(hRequest, L"Accept: application/vnd.github+json", static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

    std::string response;

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(hRequest, nullptr))
    {
        DWORD nStatus = 0;
        DWORD nStatusSize = sizeof(nStatus);

        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &nStatus, &nStatusSize, WINHTTP_NO_HEADER_INDEX) && nStatus >= 200 && nStatus < 300)
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

                // tag_name sits near the front of the reply. The release body can run long and none
                // of it is wanted.
                if (response.size() > 256 * 1024)
                    break;
            } while (nAvailable > 0);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (response.empty())
        return false;

    // One field, so a regex rather than a json dependency. Tags are published as v1.2.3 and the v
    // is not part of the version. Custom delimiter: the pattern's own )" would close a plain R"( ).
    std::smatch match;
    const std::regex tag(R"rx("tag_name"\s*:\s*"\s*[vV]?([0-9][^"]*)")rx");

    if (!std::regex_search(response, match, tag) || match.size() < 2)
        return false;

    latest = match[1];
    return true;
}

// TaskDialogIndirect is the only prompt with a tick box, and it lives in comctl32 version 6, which
// a plain LoadLibrary never reaches: system32 holds 5.82 and that does not export it. The version 6
// binding comes from an activation context built on this asi's own RT_MANIFEST, held open across
// the load and the call.
class ComCtl6Scope
{
public:
    ComCtl6Scope()
    {
        HMODULE hSelf = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&szTitle), &hSelf) || !hSelf)
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

// Yes opens the page, No closes the prompt, the tick box answers for this release under either.
// Fallback for a machine where the activation context or the export does not come together is
// MB_YESNOCANCEL with Cancel standing in for the tick box.
static bool AskAboutUpdate(const std::wstring& instruction, const std::wstring& content, const std::wstring& verification, bool& bSilence)
{
    bSilence = false;

    {
        ComCtl6Scope comctl6;
        if (comctl6.IsActive())
        {
            if (auto hComCtl = LoadLibraryW(L"comctl32.dll"))
            {
                using TaskDialogIndirect_t = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
                auto pTaskDialogIndirect = reinterpret_cast<TaskDialogIndirect_t>(GetProcAddress(hComCtl, "TaskDialogIndirect"));

                if (pTaskDialogIndirect)
                {
                    TASKDIALOGCONFIG config{};
                    config.cbSize = sizeof(config);
                    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
                    config.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
                    config.pszWindowTitle = szTitle;
                    config.pszMainIcon = TD_INFORMATION_ICON;
                    config.pszMainInstruction = instruction.c_str();
                    config.pszContent = content.c_str();
                    config.pszVerificationText = verification.c_str();

                    auto nButton = 0;
                    auto bChecked = FALSE;

                    const auto hr = pTaskDialogIndirect(&config, &nButton, nullptr, &bChecked);

                    FreeLibrary(hComCtl);

                    if (SUCCEEDED(hr))
                    {
                        bSilence = bChecked != FALSE;
                        return nButton == IDYES;
                    }

                    hComCtl = nullptr;
                }

                if (hComCtl)
                    FreeLibrary(hComCtl);
            }
        }
    }

    const auto text = instruction + L"\n\n" + content + L"\n\nCancel is the tick box: " + verification + L".";
    const auto nAnswer = MessageBoxW(nullptr, text.c_str(), szTitle, MB_YESNOCANCEL | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);

    bSilence = nAnswer == IDCANCEL;
    return nAnswer == IDYES;
}

static void ShowUpdateMessage(const std::string& installed, const std::string& latest, bool& bSilence)
{
    const auto instruction = std::wstring(L"A new version of FC2JackalFix is available.");

    const auto content = std::wstring(L"Installed: ") + FormatVersion(installed) + L"\n"
        + L"Latest: " + FormatVersion(latest) + L"\n\n"
        + L"Releases are published on GitHub at\n" + szUpdateUrl + L"\n\n"
        + L"Open the download page now?";

    const auto verification = std::wstring(L"Don't show again");

    if (!AskAboutUpdate(instruction, content, verification, bSilence))
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

        // A different release than the cached one has not been prompted for yet.
        if (CompareVersion(fetched, latest) != 0)
            mentioned.clear();

        latest = fetched;
        SaveCache(latest, mentioned, silenced);
    }

    // Installed ahead of the release is a local build, equal is up to date.
    if (CompareVersion(szInstalledVersion, latest) >= 0)
        return;

    // The tick box covers the release it was ticked for and nothing later, so V3 asks again after
    // V2 was silenced.
    if (!silenced.empty() && CompareVersion(latest, silenced) <= 0)
        return;

    if (!mentioned.empty() && CompareVersion(mentioned, latest) == 0)
        return;

    // Recorded before the prompt, so a crash or an alt-F4 while it is open does not repeat it next
    // launch.
    SaveCache(latest, latest, silenced);

    auto bSilence = false;
    ShowUpdateMessage(szInstalledVersion, latest, bSilence);

    if (bSilence)
        SaveCache(latest, latest, latest);
}

// Own thread. The request runs while the game finishes loading rather than holding onInitEvent.
class UpdateCheck
{
public:
    UpdateCheck()
    {
        JackalFix::onInitEvent() += []()
        {
            CreateThreadAutoClose(nullptr, 0, [](LPVOID) -> DWORD
            {
                CheckForUpdates();
                return 0;
            }, nullptr, 0, nullptr);
        };
    }
} UpdateCheck;
