#pragma once

namespace vkShade
{
    // Call once at startup with config value
    void initInputBlocker(bool enabled);

    void setInputBlocked(bool blocked);
    bool isInputBlocked();
}
