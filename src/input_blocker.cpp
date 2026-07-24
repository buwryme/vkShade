#include "input_blocker.hpp"
#include "wayland_display.hpp"
#include "wayland_interpose.hpp"
#include "logger.hpp"

#include <atomic>

namespace VKIntox
{
    static std::atomic<bool> blockingEnabled{false};
    // Atomic: written by overlay thread (setInputBlocked), read by game thread
    // (isInputBlocked via Wayland interpose wrapper callbacks)
    static std::atomic<bool> blocked{false};

    static void warnNonWaylandInputOnce(const char* message)
    {
        static bool warned = false;
        if (!warned && isNonWaylandSurface())
        {
            Logger::warn(message);
            warned = true;
        }
    }

    void initInputBlocker(bool enabled)
    {
        blockingEnabled = enabled;

        if (isWayland())
        {
            // Wayland doesn't support global input grabs
            // Input events are delivered to our private event queue
            // and consumed by the overlay when visible
            Logger::debug(std::string("Input blocking ") + (enabled ? "enabled (Wayland: event consumption mode)" : "disabled"));
            return;
        }

        blocked.store(false, std::memory_order_release);
        warnNonWaylandInputOnce("non-Wayland Vulkan surface: input blocking disabled; pass-through only");
        Logger::debug(std::string("Input blocking ") + (enabled ? "disabled for non-Wayland surface" : "disabled"));
    }

    void setInputBlocked(bool shouldBlock)
    {
        if (!blockingEnabled.load(std::memory_order_acquire))
            return;

        if (!isWayland())
        {
            blocked.store(false, std::memory_order_release);
            return;
        }

        if (shouldBlock == blocked.load(std::memory_order_acquire))
            return;

        blocked.store(shouldBlock, std::memory_order_release);

        // On Wayland, interposed wl_proxy_add_listener wrapper callbacks
        // check isInputBlocked() and suppress events to the game.
        // NOTE: This does NOT work for Wine Wayland games — Wine loads
        // winewayland.so via dlopen(RTLD_LOCAL), so libwayland-client
        // resolves in Wine's local scope, bypassing our LD_PRELOAD
        // interposition entirely. No workaround exists without LD_AUDIT
        // or a wrapper libwayland-client.so.
        Logger::debug(std::string("Wayland input blocking: ") + (shouldBlock ? "suppressing game events" : "forwarding game events"));
        // Send synthetic leave/enter to game keyboards so held keys
        // are released when overlay opens (prevents stuck movement/actions).
        // Only works when wl_proxy_add_listener interposition is active.
        notifyGameKeyboardFocus(!shouldBlock);
    }

    bool isInputBlocked()
    {
        return blocked.load(std::memory_order_acquire);
    }
}
