#include "mouse_input.hpp"

#include "logger.hpp"
#include "wayland_display.hpp"
#include "mouse_input_wayland.hpp"

namespace vkShade
{
    static void warnNonWaylandMouseOnce(const char* message)
    {
        static bool warned = false;
        if (!warned && isNonWaylandSurface())
        {
            Logger::warn(message);
            warned = true;
        }
    }

    MouseState getMouseState()
    {
        if (isWayland())
            return getMouseStateWayland();

        warnNonWaylandMouseOnce("non-Wayland Vulkan surface: mouse polling disabled; returning default state");
        return MouseState();
    }

} // namespace vkShade
