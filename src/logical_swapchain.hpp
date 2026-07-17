#ifndef LOGICAL_SWAPCHAIN_HPP_INCLUDED
#define LOGICAL_SWAPCHAIN_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "effects/effect.hpp"

#include "vulkan_include.hpp"

#include "logical_device.hpp"

namespace vkShade
{
    class Config;

    // for each swapchain, we have the Images and the other stuff we need to execute the compute shader
    // Per-image depth resolve resources — kept together so they
    // can never desynchronize across parallel vectors.
    struct DepthResolvePerImage
    {
        VkImage        image     = VK_NULL_HANDLE;
        VkImageView    imageView = VK_NULL_HANDLE;
        VkDeviceMemory memory    = VK_NULL_HANDLE;
        bool           initialized = false;
    };

    struct LogicalSwapchain
    {
        LogicalDevice*                       pLogicalDevice;
        VkSwapchainCreateInfoKHR             swapchainCreateInfo;
        VkExtent2D                           imageExtent;
        VkFormat                             format;
        uint32_t                             imageCount;
        bool                                 useMutableFormat = false;
        std::vector<VkImage>                 images;
        std::vector<VkImageView>             imageViews;  // for overlay rendering
        std::vector<VkImage>                 fakeImages;
        size_t                               maxEffectSlots = 0;  // Max number of effects supported
        std::vector<VkCommandBuffer>         commandBuffersEffect;
        std::vector<VkCommandBuffer>         commandBuffersNoEffect;
        std::vector<VkSemaphore>             semaphores;
        std::vector<VkSemaphore>             overlaySemaphores;
        std::vector<std::shared_ptr<Effect>> effects;
        std::shared_ptr<Effect>              defaultTransfer;
        std::vector<VkDeviceMemory>          fakeImageMemories;
        std::vector<DepthResolvePerImage>       depthResolvePerImage;
        VkFormat                             depthResolveFormat = VK_FORMAT_UNDEFINED;
        VkExtent3D                           depthResolveExtent = {0, 0, 1};
        VkImageView                          depthResolveSourceView = VK_NULL_HANDLE;  // Track which view we resolved for
        bool                                 depthResolveUsesShader = false;
        // MSAA depth resolve path. When the active depth source is multisampled
        // and not in GENERAL layout, the layer resolves it down to 1 sample via
        // a depth-stencil resolve subpass (VkSubpassDescriptionDepthStencilResolve)
        // before effects sample it. The resolve target is depthResolvePerImage.
        bool                                 depthResolveIsMsaa = false;
        VkSampleCountFlagBits                depthResolveSourceSamples = VK_SAMPLE_COUNT_1_BIT;
        VkResolveModeFlagBits                depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        VkRenderPass                         depthResolveMsaaRenderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer>           depthResolveMsaaFramebuffers;
        VkSampler                            depthResolveSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout                depthResolveDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool                     depthResolveDescriptorPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet>         depthResolveDescriptorSets;
        VkRenderPass                         depthResolveRenderPass = VK_NULL_HANDLE;
        VkPipelineLayout                     depthResolvePipelineLayout = VK_NULL_HANDLE;
        VkPipeline                           depthResolvePipeline = VK_NULL_HANDLE;
        std::vector<VkFramebuffer>           depthResolveFramebuffers;

        // Per-image fences to track effect command buffer completion.
        // CRITICAL: We must NOT update descriptor sets or free command buffers
        // while the GPU is still using them.  Each fence is signaled when the
        // corresponding effect CB (and its dependent overlay CB) is submitted.
        // Before re-recording or updating descriptor sets for imageIndex, we
        // wait on effectSubmitFences[imageIndex] to ensure the GPU finished the
        // previous frame's work.  Without this, UpdateDescriptorSets modifies a
        // descriptor set that's still bound to an in-flight CB → VK_ERROR_DEVICE_LOST.
        std::vector<VkFence>                  effectSubmitFences;

        void destroy();
        void reloadEffects(Config* pConfig);
    };
} // namespace vkShade

#endif // LOGICAL_SWAPCHAIN_HPP_INCLUDED
