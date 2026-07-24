#ifndef RENDERPASS_HPP_INCLUDED
#define RENDERPASS_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "vulkan_include.hpp"

#include "logical_device.hpp"

namespace VKIntox
{
    VkRenderPass createRenderPass(LogicalDevice* pLogicalDevice, VkFormat format, VkImageLayout finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // Render pass that resolves a multisampled depth attachment (attachment 0,
    // `samples`) into a 1-sample depth target (attachment 1) via a
    // VkSubpassDescriptionDepthStencilResolve. No draws are recorded; the
    // resolve is performed by the implementation at subpass end. Used by the
    // MSAA depth-capture path for complex renderers (e.g. Roblox). Returns
    // VK_NULL_HANDLE if the device lacks depth resolve support.
    VkRenderPass createDepthMsaaResolveRenderPass(LogicalDevice*      pLogicalDevice,
                                                  VkFormat            depthFormat,
                                                  VkSampleCountFlagBits samples,
                                                  VkResolveModeFlagBits depthResolveMode,
                                                  VkImageLayout       resolveFinalLayout);
}

#endif // RENDERPASS_HPP_INCLUDED
