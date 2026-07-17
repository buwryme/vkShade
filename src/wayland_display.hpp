#pragma once

struct wl_display;
struct wl_surface;

namespace vkShade
{
    // Called from vkCreateWaylandSurfaceKHR to capture the game's wl_display
    void setWaylandDisplay(wl_display* display);

    // Returns the captured wl_display, or nullptr if not on Wayland
    wl_display* getWaylandDisplay();

    // Called from vkCreateWaylandSurfaceKHR to capture the game's wl_surface
    void setWaylandSurface(wl_surface* surface);

    // Returns the captured wl_surface, or nullptr if not on Wayland
    wl_surface* getWaylandSurface();

    // Returns true if the game created a Wayland Vulkan surface.
    bool isWayland();

    // Returns true after an Xlib/Xcb Vulkan surface has been marked unsupported.
    bool isNonWaylandSurface();

    // Called from X11/Xwayland Vulkan surface creation wrappers to mark the
    // surface unsupported and disable processing/input interception.
    void markNonWaylandSurface(const char* source);
} // namespace vkShade
