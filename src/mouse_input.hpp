#ifndef MOUSE_INPUT_HPP_INCLUDED
#define MOUSE_INPUT_HPP_INCLUDED

#include <cstdint>

namespace vkShade
{
    struct MouseState
    {
        int x = 0;
        int y = 0;
        bool leftButton = false;
        bool rightButton = false;
        bool middleButton = false;
        float scrollDelta = 0.0f;  // Positive = up, negative = down
    };

    MouseState getMouseState();

} // namespace vkShade

#endif // MOUSE_INPUT_HPP_INCLUDED
