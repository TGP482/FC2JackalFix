module;

#include <common.hxx>
#include <stacktrace>

export module common;

export class JackalFix
{
public:
    template<typename... Args>
    class Event : public std::function<void(Args...)>
    {
    public:
        using std::function<void(Args...)>::function;

    private:
        std::vector<std::pair<int, std::function<void(Args...)>>> handlers;

    public:
        // Registration order, which is static initialisation order across modules and so not
        // something a module should rely on.
        static constexpr auto nDefaultPriority = 100;

        void operator+=(std::function<void(Args...)>&& handler)
        {
            add(std::move(handler), nDefaultPriority);
        }

        // Lower runs first. Equal priorities keep the order they were registered in.
        void add(std::function<void(Args...)>&& handler, int priority)
        {
            handlers.emplace_back(priority, std::move(handler));
        }

        void executeAll(Args... args) const
        {
            auto ordered = handlers;
            std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) { return left.first < right.first; });

            for (auto& handler : ordered)
                handler.second(args...);
        }
    };

public:
    static Event<>& onInitEvent()
    {
        static Event<> InitEvent;
        return InitEvent;
    }
    // Modal prompts that have to be answered before the game is allowed to carry on loading.
    // Runs on the main thread ahead of onInitEvent, so executeAll does not return until every
    // prompt is closed.
    static Event<>& onStartupPromptEvent()
    {
        static Event<> StartupPromptEvent;
        return StartupPromptEvent;
    }
    // Fires once Dunia.dll is mapped, where every patch target lives.
    static Event<>& onDuniaInitEvent()
    {
        static Event<> DuniaInitEvent;
        return DuniaInitEvent;
    }
    static Event<>& onIniFileChange()
    {
        static Event<> IniFileChange;
        return IniFileChange;
    }
    static Event<>& onShutdownEvent()
    {
        static Event<> ShutdownEvent;
        return ShutdownEvent;
    }
};

// Applies a setting now and again on every ini re-read. Nearly every module wants exactly this.
export inline void ApplyAndWatch(std::function<void()> fn)
{
    fn();
    JackalFix::onIniFileChange() += std::function<void()>(std::move(fn));
}

// MSVC8 std::string as the game builds it: length at +0x14, capacity at +0x18, buffer inline at
// +0x04 until it outgrows 16 bytes and +0x04 becomes a pointer to it.
export const char* ReadDuniaString(uintptr_t pStr, char* pBuf, size_t nBufSize)
{
    pBuf[0] = '\0';
    if (!pStr || nBufSize < 2 || IsBadReadPtr((void*)pStr, 0x1C))
        return pBuf;

    auto nSize = *(uint32_t*)(pStr + 0x14);
    auto nCapacity = *(uint32_t*)(pStr + 0x18);
    if (nSize > 512 || nCapacity > 0x10000)
        return pBuf;

    auto pSrc = (nCapacity >= 16) ? *(const char**)(pStr + 4) : (const char*)(pStr + 4);
    if (!pSrc || IsBadReadPtr(pSrc, nSize))
        return pBuf;

    auto n = (nSize < nBufSize - 1) ? nSize : nBufSize - 1;
    for (size_t i = 0; i < n; i++)
        pBuf[i] = pSrc[i];
    pBuf[n] = '\0';
    return pBuf;
}

export template<class T = std::filesystem::path>
T GetModulePath(HMODULE hModule)
{
    static constexpr auto INITIAL_BUFFER_SIZE = MAX_PATH;
    static constexpr auto MAX_ITERATIONS = 7;

    if constexpr (std::is_same_v<T, std::filesystem::path>)
    {
        std::u16string ret;
        std::filesystem::path pathret;
        auto bufferSize = INITIAL_BUFFER_SIZE;
        for (size_t iterations = 0; iterations < MAX_ITERATIONS; ++iterations)
        {
            ret.resize(bufferSize);
            auto charsReturned = GetModuleFileNameW(hModule, (LPWSTR)&ret[0], bufferSize);
            if (charsReturned < ret.length())
            {
                ret.resize(charsReturned);
                pathret = ret;
                return pathret;
            }
            bufferSize *= 2;
        }
    }
    else
    {
        T ret;
        auto bufferSize = INITIAL_BUFFER_SIZE;
        for (size_t iterations = 0; iterations < MAX_ITERATIONS; ++iterations)
        {
            ret.resize(bufferSize);
            size_t charsReturned = 0;
            if constexpr (std::is_same_v<T, std::string>)
                charsReturned = GetModuleFileNameA(hModule, &ret[0], bufferSize);
            else
                charsReturned = GetModuleFileNameW(hModule, &ret[0], bufferSize);
            if (charsReturned < ret.length())
            {
                ret.resize(charsReturned);
                return ret;
            }
            bufferSize *= 2;
        }
    }
    return T();
}

export template<class T = std::filesystem::path>
T GetThisModulePath()
{
    HMODULE hm = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&JackalFix::onInitEvent, &hm);
    T r = GetModulePath<T>(hm);
    if constexpr (std::is_same_v<T, std::filesystem::path>)
        return r.parent_path();
    else if constexpr (std::is_same_v<T, std::string>)
        return r.substr(0, r.find_last_of("/\\") + 1);
    else
        return r.substr(0, r.find_last_of(L"/\\") + 1);
}

export template<class T = std::filesystem::path>
T GetExeModulePath()
{
    T r = GetModulePath<T>(NULL);
    if constexpr (std::is_same_v<T, std::filesystem::path>)
        return r.parent_path();
    else if constexpr (std::is_same_v<T, std::string>)
        return r.substr(0, r.find_last_of("/\\") + 1);
    else
        return r.substr(0, r.find_last_of(L"/\\") + 1);
}

export template<class T = std::filesystem::path>
T GetExeModuleName()
{
    const T moduleFileName = GetModulePath<T>(NULL);
    if constexpr (std::is_same_v<T, std::filesystem::path>)
        return moduleFileName.filename();
    else if constexpr (std::is_same_v<T, std::string>)
        return moduleFileName.substr(moduleFileName.find_last_of("/\\") + 1);
    else
        return moduleFileName.substr(moduleFileName.find_last_of(L"/\\") + 1);
}

export inline void CreateThreadAutoClose(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId)
{
    CloseHandle(CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId));
}

export inline bool IsModuleUAL(HMODULE mod)
{
    return GetProcAddress(mod, "IsUltimateASILoader") != NULL;
}

// Walks the call stack for Ultimate ASI Loader, so the same binary works as a UAL plugin or
// self-initializes under any other loader.
export bool IsUALPresent()
{
    for (const auto& entry : std::stacktrace::current())
    {
        HMODULE hModule = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)entry.native_handle(), &hModule))
        {
            if (IsModuleUAL(hModule))
                return true;
        }
    }
    return false;
}

export class CallbackHandler
{
public:
    static inline void RegisterCallback(std::function<void()>&& fn)
    {
        fn();
    }

    static inline void RegisterCallback(std::wstring_view module_name, std::function<void()>&& fn)
    {
        if (module_name.empty() || GetModuleHandleW(module_name.data()) != NULL)
        {
            fn();
        }
        else
        {
            RegisterDllNotification();
            GetOnModuleLoadCallbackList().emplace(module_name, std::forward<std::function<void()>>(fn));
        }
    }

private:
    struct Comparator
    {
        bool operator()(const std::wstring& s1, const std::wstring& s2) const
        {
            std::wstring str1(s1.length(), ' ');
            std::wstring str2(s2.length(), ' ');
            std::transform(s1.begin(), s1.end(), str1.begin(), tolower);
            std::transform(s2.begin(), s2.end(), str2.begin(), tolower);
            return str1 < str2;
        }
    };

    static inline std::map<std::wstring, std::function<void()>, Comparator>& GetOnModuleLoadCallbackList()
    {
        static std::map<std::wstring, std::function<void()>, Comparator> onModuleLoad;
        return onModuleLoad;
    }

    static inline void invokeOnModuleLoad(std::wstring_view module_name)
    {
        auto& list = GetOnModuleLoadCallbackList();
        auto it = list.find(module_name.data());
        if (it != list.end())
            it->second();
    }

    typedef NTSTATUS(NTAPI* _LdrRegisterDllNotification)(ULONG, PVOID, PVOID, PVOID);
    typedef NTSTATUS(NTAPI* _LdrUnregisterDllNotification)(PVOID);

    typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA
    {
        ULONG Flags;
        PUNICODE_STRING FullDllName;
        PUNICODE_STRING BaseDllName;
        PVOID DllBase;
        ULONG SizeOfImage;
    } LDR_DLL_LOADED_NOTIFICATION_DATA, LDR_DLL_UNLOADED_NOTIFICATION_DATA, * PLDR_DLL_LOADED_NOTIFICATION_DATA;

    typedef union _LDR_DLL_NOTIFICATION_DATA
    {
        LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
        LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
    } LDR_DLL_NOTIFICATION_DATA, * PLDR_DLL_NOTIFICATION_DATA;

    static inline void CALLBACK LdrDllNotification(ULONG NotificationReason, PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID Context)
    {
        static constexpr auto LDR_DLL_NOTIFICATION_REASON_LOADED = 1;
        if (NotificationReason == LDR_DLL_NOTIFICATION_REASON_LOADED)
            invokeOnModuleLoad(NotificationData->Loaded.BaseDllName->Buffer);
    }

    static inline void RegisterDllNotification()
    {
        LdrRegisterDllNotification = (_LdrRegisterDllNotification)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
        if (LdrRegisterDllNotification && !cookie)
            LdrRegisterDllNotification(0, LdrDllNotification, 0, &cookie);
    }

private:
    static inline _LdrRegisterDllNotification LdrRegisterDllNotification;
    static inline void* cookie;
public:
    static inline std::once_flag flag;
};

// Returns the first pattern that resolves, so one patch body covers several game versions.
export template <size_t count = 1, typename... Args>
hook::pattern find_pattern(Args... args)
{
    hook::pattern pattern;
    ((pattern = hook::pattern(args), !pattern.count_hint(count).empty()) || ...);
    return pattern;
}

export class raw_mem
{
public:
    raw_mem(injector::memory_pointer_tr addr, std::initializer_list<uint8_t> bytes, bool offset_back = false)
    {
        ptr = addr.as_int() - (offset_back ? bytes.size() : 0);
        new_code.assign(std::move(bytes));
        old_code.resize(new_code.size());
        ReadMemoryRaw(ptr, old_code.data(), old_code.size(), true);
    }

    void Write()
    {
        WriteMemoryRaw(ptr, new_code.data(), new_code.size(), true);
    }

    void Restore()
    {
        WriteMemoryRaw(ptr, old_code.data(), old_code.size(), true);
    }

    void Set(bool bOn)
    {
        bOn ? Write() : Restore();
    }

    size_t Size() const
    {
        return old_code.size();
    }

private:
    injector::memory_pointer ptr;
    std::vector<uint8_t> old_code;
    std::vector<uint8_t> new_code;
};
