#include "memory.hpp"

namespace vkShade
{
    uint32_t findMemoryTypeIndex(LogicalDevice* pLogicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties physicalDeviceMemoryProperties;
        pLogicalDevice->vki.GetPhysicalDeviceMemoryProperties(pLogicalDevice->physicalDevice, &physicalDeviceMemoryProperties);

        // Pass 1: exact match (all requested flags present)
        for (uint32_t i = 0; i < physicalDeviceMemoryProperties.memoryTypeCount; i++)
        {
            if ((typeFilter & (1 << i)) && (physicalDeviceMemoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }

        // Pass 2: fallback — any type matching the filter bits, even without
        // all requested property flags.  Handles translation layers
        // (e.g. Android-on-Linux via Sober) that may not expose DEVICE_LOCAL
        // for certain formats or report unusual memory type configurations.
        for (uint32_t i = 0; i < physicalDeviceMemoryProperties.memoryTypeCount; i++)
        {
            if (typeFilter & (1 << i))
            {
                Logger::warn("No memory type with exact flags found; falling back to type " + std::to_string(i));
                return i;
            }
        }

        Logger::err("No memory type matching filter bits at all");
        return 0xFFFFFFFF;
    }
} // namespace vkShade
