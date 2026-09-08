// injector's memory interface, backed by FCSE instead of injector itself.
//
// Writes go through FCSE_PluginAPI::Patch so the loader owns the byte ranges and can reject a
// second plugin claiming the same ones. Reads are plain copies. Only what the feature modules use
// is here.
#pragma once

#include <fcse_api.h>

#include <cstdint>
#include <cstring>

namespace injector {

// injector distinguishes translated and raw pointers for games that relocate; Dunia does not, so
// one address type serves as both memory_pointer and memory_pointer_tr.
struct memory_pointer_tr {
    uintptr_t a = 0;

    memory_pointer_tr() = default;
    memory_pointer_tr(uintptr_t address) : a(address) {}

    template <typename T>
    memory_pointer_tr(T* pointer) : a(reinterpret_cast<uintptr_t>(pointer)) {}

    uintptr_t as_int() const { return a; }

    template <typename T = void>
    T* get() const {
        return reinterpret_cast<T*>(a);
    }

    memory_pointer_tr operator+(uintptr_t rhs) const { return memory_pointer_tr{ a + rhs }; }
    memory_pointer_tr operator-(uintptr_t rhs) const { return memory_pointer_tr{ a - rhs }; }
};

using memory_pointer = memory_pointer_tr;

// vp is injector's "unprotect first" flag. FCSE::Patch always unprotects, so it is accepted and
// ignored rather than removed from the call sites.
//
// A write of the bytes already there is dropped. raw_mem re-applies every patch each time a setting
// changes, and FCSE records a byte range and a log line per call, so passing those through would
// grow both without ever changing the game.
inline void WriteMemoryRaw(memory_pointer_tr addr, const void* value, size_t size, bool vp = false) {
    (void)vp;
    const FCSE_PluginAPI* api = FCSE::ApiPointer();

    if (api == nullptr || std::memcmp(addr.get(), value, size) == 0)
        return;

    api->Patch(addr.get(), value, size);
}

inline void ReadMemoryRaw(memory_pointer_tr addr, void* ret, size_t size, bool vp = false) {
    (void)vp;
    std::memcpy(ret, addr.get(), size);
}

template <typename T>
inline void WriteMemory(memory_pointer_tr addr, T value, bool vp = false) {
    WriteMemoryRaw(addr, &value, sizeof(T), vp);
}

template <typename T>
inline T ReadMemory(memory_pointer_tr addr, bool vp = false) {
    T value{};
    ReadMemoryRaw(addr, &value, sizeof(T), vp);
    return value;
}

} // namespace injector
