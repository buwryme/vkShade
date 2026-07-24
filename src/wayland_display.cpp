#include "wayland_display.hpp"

#include "logger.hpp"

#include <cstdlib>
#include <string>
#include <atomic>

namespace VKIntox
{
    static std::atomic<wl_display*> waylandDisplay{nullptr};
    static std::atomic<wl_surface*> waylandSurface{nullptr};
    static std::atomic<int> waylandChecked{-1}; // -1 = unchecked, 0 = no, 1 = yes
    static std::atomic<bool> nonWaylandSurfaceFlag{false};

    void setWaylandDisplay(wl_display* display)
    {
        if (!display)
            return;

        if (nonWaylandSurfaceFlag.load(std::memory_order_acquire))
            return;

        waylandDisplay.store(display, std::memory_order_release);
        waylandChecked.store(1, std::memory_order_release);
        Logger::info("captured Wayland display from vkCreateWaylandSurfaceKHR");
    }

    wl_display* getWaylandDisplay()
    {
        return waylandDisplay.load(std::memory_order_acquire);
    }

    void setWaylandSurface(wl_surface* surface)
    {
        if (!surface)
            return;

        if (nonWaylandSurfaceFlag.load(std::memory_order_acquire))
            return;

        waylandSurface.store(surface, std::memory_order_release);
        Logger::info("captured Wayland surface from vkCreateWaylandSurfaceKHR");
    }

    wl_surface* getWaylandSurface()
    {
        return waylandSurface.load(std::memory_order_acquire);
    }

    bool isWayland()
    {
        if (nonWaylandSurfaceFlag.load(std::memory_order_acquire))
            return false;

        int checked = waylandChecked.load(std::memory_order_acquire);
        if (checked >= 0)
            return checked == 1;

        // Only enable the Wayland input backend after we actually intercepted
        // vkCreateWaylandSurfaceKHR and captured the game's wl_display/surface.
        // This avoids false positives on Xwayland apps where WAYLAND_DISPLAY
        // exists in the session but Vulkan uses Xlib/Xcb surfaces.
        if (waylandDisplay.load(std::memory_order_acquire) != nullptr
            || waylandSurface.load(std::memory_order_acquire) != nullptr)
        {
            waylandChecked.store(1, std::memory_order_release);
            return true;
        }

        waylandChecked.store(0, std::memory_order_release);
        return false;
    }

    bool isNonWaylandSurface()
    {
        return nonWaylandSurfaceFlag.load(std::memory_order_acquire);
    }

    void markNonWaylandSurface(const char* source)
    {
        if (nonWaylandSurfaceFlag.load(std::memory_order_acquire))
            return;

        nonWaylandSurfaceFlag.store(true, std::memory_order_release);
        waylandDisplay.store(nullptr, std::memory_order_release);
        waylandSurface.store(nullptr, std::memory_order_release);
        waylandChecked.store(0, std::memory_order_release);
        if (source && *source)
            Logger::warn(std::string("unsupported non-Wayland Vulkan surface via ") + source + "; VKIntox will pass through only");
        else
            Logger::warn("unsupported non-Wayland Vulkan surface detected; VKIntox will pass through only");
    }
} // namespace VKIntox