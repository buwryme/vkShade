// Wayland event interposition for input blocking.
//
// On Wayland, there is no XGrabPointer equivalent. The game and our overlay
// both receive pointer/keyboard events from the compositor for the same
// surface. This module interposes on wl_proxy_add_listener and
// wl_proxy_add_dispatcher (the underlying C functions that the generated
// protocol helpers use) and wraps the game's pointer/keyboard callbacks with
// callbacks that suppress events when the overlay has input blocked.
//
// Overlay-owned proxies are registered via registerOverlayProxy() and are
// always passed through unwrapped.

#include "wayland_interpose.hpp"
#include "input_blocker.hpp"
#include "wayland_display.hpp"
#include "logger.hpp"

// Forward-declare to avoid pulling in mouse_input_wayland.hpp (which
// transitively includes mouse_input.hpp → X11 headers in some targets).
namespace VKIntox { void mirrorButtonState(uint32_t button, bool pressed); }

#include <wayland-client.h>
#include <dlfcn.h>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <cstdlib>

namespace VKIntox
{
    namespace
    {
        // Vulkan layers are typically loaded RTLD_LOCAL, which prevents global
        // symbol interposition in normal implicit-layer mode. Promote this
        // already-loaded object to RTLD_GLOBAL so wl_proxy/dlsym wrappers are
        // visible process-wide without requiring LD_PRELOAD.
        __attribute__((constructor)) void promoteSelfToGlobalScope()
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(&promoteSelfToGlobalScope), &info) == 0 || !info.dli_fname)
                return;

            void* handle = dlopen(info.dli_fname, RTLD_NOLOAD | RTLD_NOW | RTLD_GLOBAL);
            if (!handle)
                handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_GLOBAL);

            if (handle)
                Logger::debug("Wayland interpose: promoted VKIntox library to RTLD_GLOBAL");
        }
    } // namespace

    static std::mutex proxyMutex;
    static std::unordered_set<wl_proxy*> overlayProxies;

    void registerOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        overlayProxies.insert(proxy);
    }

    void unregisterOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        overlayProxies.erase(proxy);
    }

    static bool isOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        return overlayProxies.count(proxy) > 0;
    }
} // namespace VKIntox

// ── Game listener storage ────────────────────────────────────────────────────

// Per-proxy data for wrapped game listeners. We store the game's original
// listener and user_data so we can forward events when not blocking.
struct GamePointerData
{
    wl_pointer_listener original;
    void* userData;
};

struct GameKeyboardData
{
    wl_keyboard_listener original;
    void* userData;
};

static std::mutex gameDataMutex;
static std::unordered_map<wl_pointer*, GamePointerData> gamePointers;
static std::unordered_map<wl_keyboard*, GameKeyboardData> gameKeyboards;

struct GamePointerDispatcherData
{
    wl_dispatcher_func_t original;
    const void* dispatcherData;
};

struct GameKeyboardDispatcherData
{
    wl_dispatcher_func_t original;
    const void* dispatcherData;
};

static std::unordered_map<wl_proxy*, GamePointerDispatcherData> gamePointerDispatchers;
static std::unordered_map<wl_proxy*, GameKeyboardDispatcherData> gameKeyboardDispatchers;

// ── Wrapper pointer listener ─────────────────────────────────────────────────

static void wp_enter(void* data, wl_pointer* p, uint32_t serial, wl_surface* s, wl_fixed_t x, wl_fixed_t y)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.enter)
        it->second.original.enter(it->second.userData, p, serial, s, x, y);
}

static void wp_leave(void* data, wl_pointer* p, uint32_t serial, wl_surface* s)
{
    // Always forward leave — the game needs to know it lost focus
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.leave)
        it->second.original.leave(it->second.userData, p, serial, s);
}

static void wp_motion(void* data, wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.motion)
        it->second.original.motion(it->second.userData, p, time, x, y);
}

static void wp_button(void* data, wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    // Always mirror button state to overlay — the game's pointer receives
    // releases via Wayland's implicit grab that our overlay pointer never sees.
    if (VKIntox::isWayland())
        VKIntox::mirrorButtonState(button, state == WL_POINTER_BUTTON_STATE_PRESSED);

    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.button)
        it->second.original.button(it->second.userData, p, serial, time, button, state);
}

static void wp_axis(void* data, wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis)
        it->second.original.axis(it->second.userData, p, time, axis, value);
}

static void wp_frame(void* data, wl_pointer* p)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.frame)
        it->second.original.frame(it->second.userData, p);
}

static void wp_axis_source(void* data, wl_pointer* p, uint32_t source)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_source)
        it->second.original.axis_source(it->second.userData, p, source);
}

static void wp_axis_stop(void* data, wl_pointer* p, uint32_t time, uint32_t axis)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_stop)
        it->second.original.axis_stop(it->second.userData, p, time, axis);
}

static void wp_axis_discrete(void* data, wl_pointer* p, uint32_t axis, int32_t discrete)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_discrete)
        it->second.original.axis_discrete(it->second.userData, p, axis, discrete);
}

static void wp_axis_value120(void* data, wl_pointer* p, uint32_t axis, int32_t value120)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_value120)
        it->second.original.axis_value120(it->second.userData, p, axis, value120);
}

static void wp_axis_relative_direction(void* data, wl_pointer* p, uint32_t axis, uint32_t direction)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_relative_direction)
        it->second.original.axis_relative_direction(it->second.userData, p, axis, direction);
}

static const wl_pointer_listener wrapperPointerListener = {
    .enter = wp_enter,
    .leave = wp_leave,
    .motion = wp_motion,
    .button = wp_button,
    .axis = wp_axis,
    .frame = wp_frame,
    .axis_source = wp_axis_source,
    .axis_stop = wp_axis_stop,
    .axis_discrete = wp_axis_discrete,
    .axis_value120 = wp_axis_value120,
    .axis_relative_direction = wp_axis_relative_direction,
};

// ── Wrapper keyboard listener ────────────────────────────────────────────────

static void wk_keymap(void* data, wl_keyboard* kb, uint32_t format, int32_t fd, uint32_t size)
{
    // Always forward keymap — needed for initialization
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.keymap)
        it->second.original.keymap(it->second.userData, kb, format, fd, size);
}

static void wk_enter(void* data, wl_keyboard* kb, uint32_t serial, wl_surface* s, wl_array* keys)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.enter)
        it->second.original.enter(it->second.userData, kb, serial, s, keys);
}

static void wk_leave(void* data, wl_keyboard* kb, uint32_t serial, wl_surface* s)
{
    // Always forward leave
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.leave)
        it->second.original.leave(it->second.userData, kb, serial, s);
}

static void wk_key(void* data, wl_keyboard* kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    if (VKIntox::isInputBlocked())
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.key)
        it->second.original.key(it->second.userData, kb, serial, time, key, state);
}

static void wk_modifiers(void* data, wl_keyboard* kb, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    // Always forward modifiers — games need modifier state for keybinds
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.modifiers)
        it->second.original.modifiers(it->second.userData, kb, serial, depressed, latched, locked, group);
}

static void wk_repeat_info(void* data, wl_keyboard* kb, int32_t rate, int32_t delay)
{
    // Always forward repeat info
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.repeat_info)
        it->second.original.repeat_info(it->second.userData, kb, rate, delay);
}

static const wl_keyboard_listener wrapperKeyboardListener = {
    .keymap = wk_keymap,
    .enter = wk_enter,
    .leave = wk_leave,
    .key = wk_key,
    .modifiers = wk_modifiers,
    .repeat_info = wk_repeat_info,
};

// ── Wrapper dispatcher callbacks ────────────────────────────────────────────

static int wd_pointer(const void* data, void* target, uint32_t opcode, const struct wl_message* msg, union wl_argument* args)
{
    auto* proxy = static_cast<wl_proxy*>(target);

    if (opcode == 3 && VKIntox::isWayland())
        VKIntox::mirrorButtonState(args[2].u, args[3].u == WL_POINTER_BUTTON_STATE_PRESSED);

    if (VKIntox::isInputBlocked())
    {
        switch (opcode)
        {
            case 0: // enter
            case 2: // motion
            case 3: // button
            case 4: // axis
            case 5: // frame
            case 6: // axis_source
            case 7: // axis_stop
            case 8: // axis_discrete
            case 9: // axis_value120
            case 10: // axis_relative_direction
                return 0;
            case 1: // leave
            default:
                break;
        }
    }

    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointerDispatchers.find(proxy);
    if (it != gamePointerDispatchers.end() && it->second.original)
        return it->second.original(it->second.dispatcherData, target, opcode, msg, args);
    return 0;
}

static int wd_keyboard(const void* data, void* target, uint32_t opcode, const struct wl_message* msg, union wl_argument* args)
{
    auto* proxy = static_cast<wl_proxy*>(target);

    if (VKIntox::isInputBlocked())
    {
        switch (opcode)
        {
            case 1: // enter
            case 3: // key
                return 0;
            case 0: // keymap
            case 2: // leave
            case 4: // modifiers
            case 5: // repeat_info
            default:
                break;
        }
    }

    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboardDispatchers.find(proxy);
    if (it != gameKeyboardDispatchers.end() && it->second.original)
        return it->second.original(it->second.dispatcherData, target, opcode, msg, args);
    return 0;
}

// ── Symbol interposition ─────────────────────────────────────────────────────
//
// wl_pointer_add_listener is a static inline in <wayland-client-protocol.h>
// that calls wl_proxy_add_listener. We interpose on the underlying C function
// so that even inlined calls are caught.

using AddListenerFn = int (*)(struct wl_proxy*, void (**)(void), void*);
using DlsymFn = void* (*)(void*, const char*);

static DlsymFn getRealDlsym()
{
    static DlsymFn fn = nullptr;
    if (fn)
        return fn;

    // Prefer a versioned lookup to avoid self-recursion when we interpose dlsym.
    fn = reinterpret_cast<DlsymFn>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
    if (!fn)
        fn = reinterpret_cast<DlsymFn>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34"));
    return fn;
}

template <typename FnType>
static FnType resolveWaylandSymbol(const char* symbol)
{
    DlsymFn realDlsym = getRealDlsym();
    if (!realDlsym)
        return nullptr;

    // Preferred path for normal implicit-layer usage.
    void* sym = realDlsym(RTLD_NEXT, symbol);
    if (sym)
        return reinterpret_cast<FnType>(sym);

    // Fallback for preload / early-load scenarios where RTLD_NEXT may not
    // reach libwayland-client yet.
    static void* waylandHandle = nullptr;
    if (!waylandHandle)
    {
        waylandHandle = dlopen("libwayland-client.so.0", RTLD_NOLOAD | RTLD_NOW);
        if (!waylandHandle)
            waylandHandle = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    }

    if (waylandHandle)
    {
        sym = realDlsym(waylandHandle, symbol);
        if (sym)
            return reinterpret_cast<FnType>(sym);
    }

    return nullptr;
}

static AddListenerFn getRealAddListener()
{
    static AddListenerFn fn = resolveWaylandSymbol<AddListenerFn>("wl_proxy_add_listener");
    return fn;
}

using AddDispatcherFn = int (*)(struct wl_proxy*, wl_dispatcher_func_t, const void*, void*);
using MarshalArrayFlagsFn = wl_proxy* (*)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, uint32_t, union wl_argument*);

static AddDispatcherFn getRealAddDispatcher()
{
    static AddDispatcherFn fn = resolveWaylandSymbol<AddDispatcherFn>("wl_proxy_add_dispatcher");
    return fn;
}

static MarshalArrayFlagsFn getRealMarshalArrayFlags()
{
    static MarshalArrayFlagsFn fn = resolveWaylandSymbol<MarshalArrayFlagsFn>("wl_proxy_marshal_array_flags");
    return fn;
}

extern "C" __attribute__((visibility("default")))
int wl_proxy_add_listener(struct wl_proxy* proxy,
                          void (**implementation)(void),
                          void* data)
{
    AddListenerFn real = getRealAddListener();
    if (!real)
        return -1;

    const char* cls = wl_proxy_get_class(proxy);
    if (!cls)
        return real(proxy, implementation, data);

    // Skip overlay-owned proxies — they use their own listeners
    if (VKIntox::isOverlayProxy(proxy))
        return real(proxy, implementation, data);

    if (std::strcmp(cls, "wl_pointer") == 0)
    {
        auto* gameListener = (const wl_pointer_listener*)implementation;
        auto* ptr = (wl_pointer*)proxy;

        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            GamePointerData gpd;
            gpd.original = *gameListener;
            gpd.userData = data;
            gamePointers[ptr] = gpd;
        }

        VKIntox::Logger::debug("Wayland interpose: wrapped game pointer listener");
        return real(proxy, (void (**)(void))&wrapperPointerListener, nullptr);
    }

    if (std::strcmp(cls, "wl_keyboard") == 0)
    {
        auto* gameListener = (const wl_keyboard_listener*)implementation;
        auto* kb = (wl_keyboard*)proxy;

        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            GameKeyboardData gkd;
            gkd.original = *gameListener;
            gkd.userData = data;
            gameKeyboards[kb] = gkd;
        }

        VKIntox::Logger::debug("Wayland interpose: wrapped game keyboard listener");
        return real(proxy, (void (**)(void))&wrapperKeyboardListener, nullptr);
    }

    return real(proxy, implementation, data);
}

extern "C" __attribute__((visibility("default")))
int wl_proxy_add_dispatcher(struct wl_proxy* proxy,
                            wl_dispatcher_func_t dispatcher_func,
                            const void* dispatcher_data,
                            void* data);

extern "C" __attribute__((visibility("default")))
wl_proxy* wl_proxy_marshal_array_flags(struct wl_proxy* proxy,
                                       uint32_t opcode,
                                       const struct wl_interface* iface,
                                       uint32_t version,
                                       uint32_t flags,
                                       union wl_argument* args)
{
    MarshalArrayFlagsFn real = getRealMarshalArrayFlags();
    if (!real)
        return nullptr;

    // Block compositor window move/resize requests while overlay input is blocked.
    // xdg_toplevel requests:
    //   4: show_window_menu
    //   5: move
    //   6: resize
    const char* cls = wl_proxy_get_class(proxy);
    if (VKIntox::isInputBlocked() && cls && std::strcmp(cls, "xdg_toplevel") == 0 &&
        (opcode == 4 || opcode == 5 || opcode == 6))
    {
        return nullptr;
    }

    return real(proxy, opcode, iface, version, flags, args);
}

extern "C" __attribute__((visibility("default")))
void* dlsym(void* handle, const char* symbol)
{
    DlsymFn realDlsym = getRealDlsym();
    if (!realDlsym)
        return nullptr;

    void* resolved = realDlsym(handle, symbol);

    // Keep behavior unchanged unless VKIntox is explicitly enabled.
    // Cache the check — this function is called for every dlsym in the process.
    static const bool VKIntoxEnabled = []{
        const char* e = std::getenv("ENABLE_VKINTOX");
        return e && std::strcmp(e, "1") == 0;
    }();
    if (!VKIntoxEnabled || !resolved)
        return resolved;

    bool wantsListener = (std::strcmp(symbol, "wl_proxy_add_listener") == 0);
    bool wantsDispatcher = (std::strcmp(symbol, "wl_proxy_add_dispatcher") == 0);
    bool wantsMarshalArrayFlags = (std::strcmp(symbol, "wl_proxy_marshal_array_flags") == 0);
    if (!wantsListener && !wantsDispatcher && !wantsMarshalArrayFlags)
        return resolved;

    Dl_info info{};
    if (dladdr(resolved, &info) == 0 || !info.dli_fname)
        return resolved;

    std::string module(info.dli_fname);
    std::transform(module.begin(), module.end(), module.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Only redirect lookups that currently resolve to libwayland-client.
    if (module.find("libwayland-client") == std::string::npos)
    {
        return resolved;
    }

    if (wantsListener)
        return reinterpret_cast<void*>(&wl_proxy_add_listener);
    if (wantsDispatcher)
        return reinterpret_cast<void*>(&wl_proxy_add_dispatcher);
    return reinterpret_cast<void*>(&wl_proxy_marshal_array_flags);
}

extern "C" __attribute__((visibility("default")))
int wl_proxy_add_dispatcher(struct wl_proxy* proxy,
                            wl_dispatcher_func_t dispatcher_func,
                            const void* dispatcher_data,
                            void* data)
{
    AddDispatcherFn real = getRealAddDispatcher();
    if (!real)
        return -1;

    const char* cls = wl_proxy_get_class(proxy);
    if (!cls)
        return real(proxy, dispatcher_func, dispatcher_data, data);

    // Skip overlay-owned proxies — they use their own dispatchers
    if (VKIntox::isOverlayProxy(proxy))
        return real(proxy, dispatcher_func, dispatcher_data, data);

    if (std::strcmp(cls, "wl_pointer") == 0)
    {
        auto* ptr = static_cast<wl_proxy*>(proxy);

        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            GamePointerDispatcherData gpd;
            gpd.original = dispatcher_func;
            gpd.dispatcherData = dispatcher_data;
            gamePointerDispatchers[ptr] = gpd;
        }

        VKIntox::Logger::debug("Wayland interpose: wrapped game pointer dispatcher");
        return real(proxy, wd_pointer, nullptr, data);
    }

    if (std::strcmp(cls, "wl_keyboard") == 0)
    {
        auto* kb = static_cast<wl_proxy*>(proxy);

        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            GameKeyboardDispatcherData gkd;
            gkd.original = dispatcher_func;
            gkd.dispatcherData = dispatcher_data;
            gameKeyboardDispatchers[kb] = gkd;
        }

        VKIntox::Logger::debug("Wayland interpose: wrapped game keyboard dispatcher");
        return real(proxy, wd_keyboard, nullptr, data);
    }

    return real(proxy, dispatcher_func, dispatcher_data, data);
}

namespace VKIntox
{
    void notifyGameKeyboardFocus(bool hasFocus)
    {
        std::lock_guard<std::mutex> lock(gameDataMutex);
        if (gameKeyboards.empty())
            return;

        for (auto& [kb, data] : gameKeyboards)
        {
            if (hasFocus)
            {
                // Synthetic enter — game regains keyboard focus with no pressed keys
                if (data.original.enter)
                {
                    wl_array emptyKeys;
                    wl_array_init(&emptyKeys);
                    data.original.enter(data.userData, kb, 0, nullptr, &emptyKeys);
                    wl_array_release(&emptyKeys);
                }
            }
            else
            {
                // Synthetic leave — game drops all held keys
                if (data.original.leave)
                    data.original.leave(data.userData, kb, 0, nullptr);
            }
        }

        Logger::debug(std::string("Wayland interpose: synthetic keyboard ") +
                       (hasFocus ? "enter" : "leave") + " sent to " +
                       std::to_string(gameKeyboards.size()) + " game keyboard(s)");
    }
} // namespace VKIntox
