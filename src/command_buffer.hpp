#ifndef COMMAND_BUFFER_HPP_INCLUDED
#define COMMAND_BUFFER_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "vulkan_include.hpp"

#include "logical_device.hpp"

#include "effects/effect.hpp"
namespace vkShade
{
    struct LogicalSwapchain;

    std::vector<VkCommandBuffer> allocateCommandBuffer(LogicalDevice* pLogicalDevice, uint32_t count);

    void writeCommandBuffers(LogicalDevice*                                 pLogicalDevice,
                             LogicalSwapchain*                              pLogicalSwapchain,
                             std::vector<std::shared_ptr<vkShade::Effect>> effects,
                             std::vector<VkCommandBuffer>                   commandBuffers,
                             const DepthState&                              depthState);

    void recordDepthResolveSnapshot(LogicalDevice*            pLogicalDevice,
                                    LogicalSwapchain*         pLogicalSwapchain,
                                    VkCommandBuffer           commandBuffer,
                                    uint32_t                  imageIndex,
                                    const DepthState&         depthState);

    std::vector<VkSemaphore> createSemaphores(LogicalDevice* pLogicalDevice, uint32_t count);
} // namespace vkShade

#endif // COMMAND_BUFFER_HPP_INCLUDED
