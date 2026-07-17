#include "keyboard_input.hpp"

#include "logger.hpp"
#include "wayland_display.hpp"

#include "keyboard_input_wayland.hpp"

namespace vkShade
{
    static void warnNonWaylandKeyboardOnce(const char* message)
    {
        static bool warned = false;
        if (!warned && isNonWaylandSurface())
        {
            Logger::warn(message);
            warned = true;
        }
    }

    uint32_t convertToKeySym(std::string key)
    {
        if (isWayland())
            return convertToKeySymWayland(key);
        return 0u;
    }

    void beginKeyboardInputFrame()
    {
        if (isWayland())
            return;
    }

    bool isKeyPressed(uint32_t ks)
    {
        if (isWayland())
            return isKeyPressedWayland(ks);

        warnNonWaylandKeyboardOnce("non-Wayland Vulkan surface: keyboard polling disabled; returning no input");
        return false;
    }

    KeyboardState getKeyboardState()
    {
        if (isWayland())
            return getKeyboardStateWayland();

        warnNonWaylandKeyboardOnce("non-Wayland Vulkan surface: keyboard polling disabled; returning default state");
        return KeyboardState();
    }
} // namespace vkShade
