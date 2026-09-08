// safetyhook's interface, backed by FCSE instead of safetyhook itself.
//
// The feature modules are written against safetyhook and stay that way; under FCSE the loader owns
// every hook so that it can arbitrate between plugins and report conflicts. Only what those modules
// use is here.
#pragma once

#include <fcse_api.h>

#include <cstdint>

namespace safetyhook {

using Xmm = FCSE_Xmm;
using Context32 = FCSE_MidHookContext;
using Context = Context32;
using MidHookFn = void (*)(Context& ctx);

// A detour, holding the trampoline FCSE handed back. Calling through it when the hook was rejected
// returns a value-initialised result rather than jumping to null, which is what safetyhook does too.
class InlineHook {
public:
    InlineHook() = default;
    explicit InlineHook(void* original) : m_original(original) {}

    explicit operator bool() const { return m_original != nullptr; }

    template <typename T>
    T original() const {
        return reinterpret_cast<T>(m_original);
    }

    template <typename RetT = void, typename... Args>
    RetT call(Args... args) {
        return m_original ? original<RetT (*)(Args...)>()(args...) : RetT();
    }

    template <typename RetT = void, typename... Args>
    RetT ccall(Args... args) {
        return m_original ? original<RetT(__cdecl*)(Args...)>()(args...) : RetT();
    }

    template <typename RetT = void, typename... Args>
    RetT thiscall(Args... args) {
        return m_original ? original<RetT(__thiscall*)(Args...)>()(args...) : RetT();
    }

    template <typename RetT = void, typename... Args>
    RetT stdcall(Args... args) {
        return m_original ? original<RetT(__stdcall*)(Args...)>()(args...) : RetT();
    }

    template <typename RetT = void, typename... Args>
    RetT fastcall(Args... args) {
        return m_original ? original<RetT(__fastcall*)(Args...)>()(args...) : RetT();
    }

private:
    void* m_original = nullptr;
};

// FCSE keeps mid-hooks for the life of the process and exposes no handle, so this carries only
// whether the hook took. Destroying it unhooks nothing, which is why no module may rely on that.
class MidHook {
public:
    MidHook() = default;
    explicit MidHook(bool installed) : m_installed(installed) {}

    explicit operator bool() const { return m_installed; }

private:
    bool m_installed = false;
};

inline InlineHook create_inline(void* target, void* destination) {
    void* original = nullptr;
    const FCSE_PluginAPI* api = FCSE::ApiPointer();

    if (api == nullptr || !api->Hook(target, destination, &original))
        return InlineHook{};

    return InlineHook{ original };
}

template <typename T, typename U>
InlineHook create_inline(T target, U destination) {
    return create_inline(reinterpret_cast<void*>(target), reinterpret_cast<void*>(destination));
}

inline MidHook create_mid(void* target, MidHookFn destination) {
    const FCSE_PluginAPI* api = FCSE::ApiPointer();
    const auto handler = reinterpret_cast<FCSE_MidHookHandler>(destination);

    return MidHook{ api != nullptr && api->MidHook(target, handler) };
}

template <typename T>
MidHook create_mid(T target, MidHookFn destination) {
    return create_mid(reinterpret_cast<void*>(target), destination);
}

} // namespace safetyhook

using SafetyHookContext = safetyhook::Context;
using SafetyHookInline = safetyhook::InlineHook;
using SafetyHookMid = safetyhook::MidHook;
