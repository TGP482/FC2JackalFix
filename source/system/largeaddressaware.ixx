module;

#include <common.hxx>

export module largeaddressaware;

import common;
import settings;

static constexpr auto szTitle = L"FC2JackalFix";

// <exe>.bak: the executable before the last change here, in either direction.
static constexpr auto szBackupSuffix = L".bak";
static constexpr auto szStagingSuffix = L".new";

// The checksum the loader validates.
static uint32_t CalcChecksum(uint32_t checksum, const void* data, size_t length)
{
    if (length && data != nullptr)
    {
        auto words = static_cast<const uint16_t*>(data);
        uint32_t sum = 0;
        do
        {
            sum = *words++ + checksum;
            checksum = static_cast<uint16_t>(sum) + (sum >> 16);
        } while (--length);
    }

    return checksum + (checksum >> 16);
}

// 32-bit PE only, validated before modifying.
static PIMAGE_NT_HEADERS32 GetNtHeaders(std::vector<uint8_t>& image)
{
    if (image.size() < sizeof(IMAGE_DOS_HEADER))
        return nullptr;

    auto pDosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(image.data());
    if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE || pDosHeader->e_lfanew <= 0)
        return nullptr;

    if (static_cast<size_t>(pDosHeader->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > image.size())
        return nullptr;

    auto pNtHeaders = reinterpret_cast<PIMAGE_NT_HEADERS32>(image.data() + pDosHeader->e_lfanew);
    if (pNtHeaders->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    if (pNtHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
        return nullptr;

    if (pNtHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return nullptr;

    return pNtHeaders;
}

static bool IsLargeAddressAware(PIMAGE_NT_HEADERS32 pNtHeaders)
{
    return (pNtHeaders->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) == IMAGE_FILE_LARGE_ADDRESS_AWARE;
}

// The running image, which is what the cap applies to.
static bool ProcessIsLargeAddressAware()
{
    auto moduleBase = reinterpret_cast<PBYTE>(GetModuleHandleW(nullptr));
    if (!moduleBase)
        return false;

    auto pDosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(moduleBase);
    auto pNtHeaders = reinterpret_cast<PIMAGE_NT_HEADERS32>(moduleBase + pDosHeader->e_lfanew);

    return IsLargeAddressAware(pNtHeaders);
}

static void SetChecksum(std::vector<uint8_t>& image)
{
    auto pNtHeaders = GetNtHeaders(image);
    if (!pNtHeaders)
        return;

    // CheckSum counts as zero, so skip it.
    const auto nHeaderSize = reinterpret_cast<uintptr_t>(&pNtHeaders->OptionalHeader.CheckSum) - reinterpret_cast<uintptr_t>(image.data());
    const auto nRemainSize = (image.size() - nHeaderSize - sizeof(uint32_t)) / sizeof(uint16_t);

    auto checksum = CalcChecksum(0, image.data(), nHeaderSize / sizeof(uint16_t));
    checksum = CalcChecksum(checksum, &pNtHeaders->OptionalHeader.Subsystem, nRemainSize);

    // Odd sized file leaves a trailing byte the word loop never reaches.
    if (image.size() & 1)
        checksum += image.back();

    pNtHeaders->OptionalHeader.CheckSum = checksum + static_cast<uint32_t>(image.size());
}

static bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    // The exe is open as an image section, so share reads or this fails.
    auto hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    auto bResult = GetFileSizeEx(hFile, &size) != FALSE;
    bResult = bResult && size.QuadPart > 0 && size.QuadPart < 512ll * 1024 * 1024;

    if (bResult)
    {
        try
        {
            out.resize(static_cast<size_t>(size.QuadPart));
        }
        catch (...)
        {
            bResult = false;
        }
    }

    if (bResult)
    {
        DWORD nRead = 0;
        bResult = ReadFile(hFile, out.data(), static_cast<DWORD>(out.size()), &nRead, nullptr) != FALSE && nRead == out.size();
    }

    CloseHandle(hFile);
    return bResult;
}

static bool WriteFileBytes(const std::filesystem::path& path, const std::vector<uint8_t>& data)
{
    auto hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD nWritten = 0;
    auto bResult = WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()), &nWritten, nullptr) != FALSE && nWritten == data.size();
    bResult = bResult && FlushFileBuffers(hFile) != FALSE;

    CloseHandle(hFile);

    if (!bResult)
        DeleteFileW(path.c_str());

    return bResult;
}

// A running executable cannot be overwritten, only renamed: build alongside and swap.
static bool SwapExecutable(const std::filesystem::path& exePath, const std::vector<uint8_t>& image)
{
    auto stagingPath = exePath;
    stagingPath += szStagingSuffix;

    auto backupPath = exePath;
    backupPath += szBackupSuffix;

    DeleteFileW(stagingPath.c_str());

    if (!WriteFileBytes(stagingPath, image))
        return false;

    if (!MoveFileExW(exePath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(stagingPath.c_str());
        return false;
    }

    if (!MoveFileExW(stagingPath.c_str(), exePath.c_str(), 0))
    {
        // Put the original back rather than leave no executable at all.
        MoveFileExW(backupPath.c_str(), exePath.c_str(), 0);
        DeleteFileW(stagingPath.c_str());
        return false;
    }

    return true;
}

// True when backup and current differ only by our own changes.
static bool BackupMatches(std::vector<uint8_t> backup, std::vector<uint8_t> current)
{
    if (backup.size() != current.size())
        return false;

    auto pBackupHeaders = GetNtHeaders(backup);
    auto pCurrentHeaders = GetNtHeaders(current);
    if (!pBackupHeaders || !pCurrentHeaders)
        return false;

    pBackupHeaders->FileHeader.Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
    pCurrentHeaders->FileHeader.Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
    pBackupHeaders->OptionalHeader.CheckSum = 0;
    pCurrentHeaders->OptionalHeader.CheckSum = 0;

    return backup == current;
}

static void WriteIniSetting(int32_t nValue)
{
    CIniReader iniReader("");
    iniReader.WriteInteger("General", "LargeAddressAware", nValue);
    JackalFixSettings.SetInt(PREF_LARGEADDRESSAWARE, nValue);
}

static int ShowMessage(const std::wstring& text, UINT flags)
{
    return MessageBoxW(nullptr, text.c_str(), szTitle, flags | MB_TOPMOST | MB_SETFOREGROUND);
}

static void ApplyLargeAddressAware()
{
    const auto bWanted = JackalFixSettings.GetInt(PREF_LARGEADDRESSAWARE) != 0;

    if (bWanted == ProcessIsLargeAddressAware())
        return;

    const auto exePath = GetModulePath<std::filesystem::path>(nullptr);
    if (exePath.empty())
        return;

    const auto exeName = exePath.filename().wstring();

    auto backupPath = exePath;
    backupPath += szBackupSuffix;

    std::vector<uint8_t> image;
    if (!ReadFileBytes(exePath, image))
        return;

    auto pNtHeaders = GetNtHeaders(image);
    if (!pNtHeaders)
        return;

    // The file can already match while the process still runs the old image.
    if (IsLargeAddressAware(pNtHeaders) == bWanted)
        return;

    if (bWanted)
    {
        const auto prompt = exeName + L" is not marked large address aware, so Windows caps the game at 2 GB of memory even on a 64 bit system. Setting the flag raises that to 4 GB.\n\n"
            L"FC2JackalFix can set it now. The current executable is kept as " + exeName + szBackupSuffix + L".\n\n"
            L"The flag is only read when the process starts, so the game has to close for this to take effect.\n\n"
            L"Patch " + exeName + L" now?\n\n"
            L"Choosing No sets LargeAddressAware to 0 in FC2JackalFix.ini, so this is not asked again.";

        if (ShowMessage(prompt, MB_YESNO | MB_ICONQUESTION) != IDYES)
        {
            WriteIniSetting(0);
            return;
        }

        pNtHeaders->FileHeader.Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
        SetChecksum(image);

        if (!SwapExecutable(exePath, image))
        {
            ShowMessage(exeName + L" could not be patched.\n\n"
                L"The game folder is most likely read only or needs administrator rights. Moving the install out of Program Files, or running the game once as administrator, usually sorts it.\n\n"
                L"LargeAddressAware has been left on, so this will be offered again next launch.", MB_OK | MB_ICONEXCLAMATION);
            return;
        }

        ShowMessage(exeName + L" is now large address aware, and the original is kept as " + exeName + szBackupSuffix + L".\n\n"
            L"The game closes when you press OK. Please relaunch the game.", MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        // Only restore a backup this made, so user changes survive.
        std::vector<uint8_t> backup;
        if (!ReadFileBytes(backupPath, backup))
            return;

        const auto prompt = L"LargeAddressAware is off in FC2JackalFix.ini, but " + exeName + L" is still patched.\n\n"
            L"FC2JackalFix can restore the unpatched executable now. The current one is kept as " + exeName + szBackupSuffix + L".\n\n"
            L"The game has to close for this to take effect.\n\n"
            L"Restore " + exeName + L" now?\n\n"
            L"Choosing No sets LargeAddressAware back to 1 in FC2JackalFix.ini, so this is not asked again.";

        if (ShowMessage(prompt, MB_YESNO | MB_ICONQUESTION) != IDYES)
        {
            WriteIniSetting(1);
            return;
        }

        if (!BackupMatches(backup, image))
        {
            // Replace stale backups with the current image.
            backup = image;
            auto pBackupHeaders = GetNtHeaders(backup);
            if (!pBackupHeaders)
                return;

            pBackupHeaders->FileHeader.Characteristics &= ~IMAGE_FILE_LARGE_ADDRESS_AWARE;
            SetChecksum(backup);
        }

        if (!SwapExecutable(exePath, backup))
        {
            ShowMessage(exeName + L" could not be restored.\n\n"
                L"The game folder is most likely read only or needs administrator rights.\n\n"
                L"LargeAddressAware has been left off, so this will be offered again next launch.", MB_OK | MB_ICONEXCLAMATION);
            return;
        }

        ShowMessage(exeName + L" has been restored, and the patched copy is kept as " + exeName + szBackupSuffix + L".\n\n"
            L"The game closes when you press OK. Please relaunch the game.", MB_OK | MB_ICONINFORMATION);
    }

    // Not exit(): the game is still initialising on the main thread.
    TerminateProcess(GetCurrentProcess(), 0);
}

// After the update prompt, on the main thread so loading waits for the answer.
static constexpr auto nStartupPromptPriority = 20;

class LargeAddressAware
{
public:
    LargeAddressAware()
    {
        JackalFix::onStartupPromptEvent().add([]()
        {
            ApplyLargeAddressAware();
        }, nStartupPromptPriority);
    }
} LargeAddressAware;
