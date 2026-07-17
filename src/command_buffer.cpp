#include "command_buffer.hpp"

#include <algorithm>

#include "format.hpp"
#include "image_view.hpp"
#include "logical_swapchain.hpp"
#include "settings_manager.hpp"
#include "util.hpp"

namespace vkShade
{
    static bool hasDepthResolveResources(const LogicalSwapchain* pLogicalSwapchain, size_t commandBufferCount)
    {
        return pLogicalSwapchain != nullptr
               && pLogicalSwapchain->depthResolvePerImage.size() == commandBufferCount;
    }

    static bool hasDepthState(const DepthState& state)
    {
        return state.image != VK_NULL_HANDLE && state.imageView != VK_NULL_HANDLE && state.format != VK_FORMAT_UNDEFINED;
    }

    static VkImageLayout getObservedDepthAttachmentLayout(const DepthState& depthState)
    {
        return depthState.observedLayout != VK_IMAGE_LAYOUT_UNDEFINED
            ? depthState.observedLayout
            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    static VkPipelineStageFlags getObservedDepthSourceStages(const DepthState& depthState)
    {
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL)
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }

    static VkAccessFlags getObservedDepthSourceAccess(const DepthState& depthState)
    {
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL)
            return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    static VkPipelineStageFlags getObservedDepthRestoreStages(const DepthState& depthState)
    {
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL)
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }

    static VkAccessFlags getObservedDepthRestoreAccess(const DepthState& depthState)
    {
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL)
            return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    static bool shouldKeepObservedDepthInGeneralLayout(const DepthState& depthState)
    {
        return getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL;
    }

    static VkImageLayout getInternalDepthReadOnlyLayout(VkFormat format)
    {
        return isStencilFormat(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    }

    static VkImageLayout getDepthResolveReadOnlyLayout(const LogicalSwapchain* pLogicalSwapchain)
    {
        if (!pLogicalSwapchain)
            return VK_IMAGE_LAYOUT_UNDEFINED;

        return pLogicalSwapchain->depthResolveUsesShader
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : getInternalDepthReadOnlyLayout(pLogicalSwapchain->depthResolveFormat);
    }

    static VkImageView getOrCreateTrackedDepthSampleView(LogicalDevice* pLogicalDevice, const DepthState& depthState)
    {
        if (!pLogicalDevice || !hasDepthState(depthState))
            return VK_NULL_HANDLE;

        auto it = pLogicalDevice->depthViewStates.find(depthState.imageView);
        if (it != pLogicalDevice->depthViewStates.end() && it->second.imageView != VK_NULL_HANDLE)
            return it->second.imageView;

        // Check if this is the persistent storage image — if so, we can't use
        // the parallel-indexed depthImages/depthFormats/depthImageViews vectors
        // (those are for app images only). Return the view directly.
        if (depthState.image == pLogicalDevice->depthCaptureStorage.image
            && pLogicalDevice->persistentStorageTracked)
        {
            return depthState.imageView;
        }

        auto imageIt = std::find(pLogicalDevice->depthImages.begin(), pLogicalDevice->depthImages.end(), depthState.image);
        if (imageIt == pLogicalDevice->depthImages.end())
            return depthState.imageView;

        const size_t index = std::distance(pLogicalDevice->depthImages.begin(), imageIt);
        // Bounds-check: the parallel vectors must have the same length.
        if (index >= pLogicalDevice->depthFormats.size() || index >= pLogicalDevice->depthImageViews.size())
        {
            Logger::warn("getOrCreateTrackedDepthSampleView: index " + std::to_string(index)
                         + " out of range (depthFormats=" + std::to_string(pLogicalDevice->depthFormats.size())
                         + " depthImageViews=" + std::to_string(pLogicalDevice->depthImageViews.size())
                         + "); returning app view");
            return depthState.imageView;
        }

        VkImageView& trackedView = pLogicalDevice->depthImageViews[index];
        if (trackedView == VK_NULL_HANDLE)
            trackedView = createImageViews(pLogicalDevice, depthState.format, {depthState.image}, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT)[0];

        return trackedView != VK_NULL_HANDLE ? trackedView : depthState.imageView;
    }

    static void recordDepthResolveSnapshotViaShader(LogicalDevice*    pLogicalDevice,
                                                    LogicalSwapchain* pLogicalSwapchain,
                                                    VkCommandBuffer   commandBuffer,
                                                    uint32_t          imageIndex,
                                                    const DepthState& depthState)
    {
        if (!pLogicalDevice || !pLogicalSwapchain || imageIndex >= pLogicalSwapchain->depthResolveDescriptorSets.size()
            || imageIndex >= pLogicalSwapchain->depthResolveFramebuffers.size())
            return;

        // CRITICAL: Wait for this imageIndex's previous effect CB to finish
        // BEFORE updating its descriptor set.  The descriptor set is still
        // "in use" by the GPU until the CB that sampled from it completes.
        // Without this fence wait, UpdateDescriptorSets modifies memory that
        // the GPU may be reading → undefined behavior → VK_ERROR_DEVICE_LOST.
        if (imageIndex < pLogicalSwapchain->effectSubmitFences.size()
            && pLogicalSwapchain->effectSubmitFences[imageIndex] != VK_NULL_HANDLE)
        {
            VkResult waitResult = pLogicalDevice->vkd.WaitForFences(
                pLogicalDevice->device, 1, &pLogicalSwapchain->effectSubmitFences[imageIndex],
                VK_TRUE, UINT64_MAX);  // Must complete before we can safely update
            if (waitResult == VK_ERROR_DEVICE_LOST)
            {
                Logger::err("recordDepthResolveSnapshotViaShader: WaitForFences returned DEVICE_LOST for image "
                            + std::to_string(imageIndex) + " — descriptor set update skipped");
                return;  // Don't record anything if device is lost
            }
            pLogicalDevice->vkd.ResetFences(pLogicalDevice->device, 1, &pLogicalSwapchain->effectSubmitFences[imageIndex]);
        }

        VkDescriptorImageInfo imageInfo = {};
        imageInfo.sampler = pLogicalSwapchain->depthResolveSampler;
        imageInfo.imageView = getOrCreateTrackedDepthSampleView(pLogicalDevice, depthState);
        imageInfo.imageLayout = shouldKeepObservedDepthInGeneralLayout(depthState)
            ? VK_IMAGE_LAYOUT_GENERAL
            : getInternalDepthReadOnlyLayout(depthState.format);

        VkWriteDescriptorSet writeDescriptorSet = {};
        writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptorSet.dstSet = pLogicalSwapchain->depthResolveDescriptorSets[imageIndex];
        writeDescriptorSet.dstBinding = 0;
        writeDescriptorSet.descriptorCount = 1;
        writeDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writeDescriptorSet.pImageInfo = &imageInfo;
        pLogicalDevice->vkd.UpdateDescriptorSets(pLogicalDevice->device, 1, &writeDescriptorSet, 0, nullptr);

        VkImageMemoryBarrier sourceBarrier = {};
        sourceBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sourceBarrier.image = depthState.image;
        sourceBarrier.oldLayout = getObservedDepthAttachmentLayout(depthState);
        sourceBarrier.newLayout = shouldKeepObservedDepthInGeneralLayout(depthState)
            ? VK_IMAGE_LAYOUT_GENERAL
            : getInternalDepthReadOnlyLayout(depthState.format);
        sourceBarrier.srcAccessMask = getObservedDepthSourceAccess(depthState);
        sourceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.subresourceRange.aspectMask =
            isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
        sourceBarrier.subresourceRange.baseMipLevel = 0;
        sourceBarrier.subresourceRange.levelCount = 1;
        sourceBarrier.subresourceRange.baseArrayLayer = 0;
        sourceBarrier.subresourceRange.layerCount = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               getObservedDepthSourceStages(depthState),
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &sourceBarrier);

        VkImageMemoryBarrier resolveBarrier = {};
        resolveBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        resolveBarrier.image = pLogicalSwapchain->depthResolvePerImage[imageIndex].image;
        resolveBarrier.oldLayout = pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
        resolveBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        resolveBarrier.srcAccessMask = pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        resolveBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        resolveBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveBarrier.subresourceRange.baseMipLevel = 0;
        resolveBarrier.subresourceRange.levelCount = 1;
        resolveBarrier.subresourceRange.baseArrayLayer = 0;
        resolveBarrier.subresourceRange.layerCount = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized
                                                   ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        VkRenderPassBeginInfo renderPassBeginInfo = {};
        renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.renderPass = pLogicalSwapchain->depthResolveRenderPass;
        renderPassBeginInfo.framebuffer = pLogicalSwapchain->depthResolveFramebuffers[imageIndex];
        renderPassBeginInfo.renderArea.offset = {0, 0};
        renderPassBeginInfo.renderArea.extent = {pLogicalSwapchain->depthResolveExtent.width, pLogicalSwapchain->depthResolveExtent.height};
        VkClearValue clearValue = {};
        clearValue.color.float32[0] = 1.0f;
        renderPassBeginInfo.clearValueCount = 1;
        renderPassBeginInfo.pClearValues = &clearValue;

        pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        pLogicalDevice->vkd.CmdBindDescriptorSets(commandBuffer,
                                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  pLogicalSwapchain->depthResolvePipelineLayout,
                                                  0,
                                                  1,
                                                  &pLogicalSwapchain->depthResolveDescriptorSets[imageIndex],
                                                  0,
                                                  nullptr);
        pLogicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pLogicalSwapchain->depthResolvePipeline);
        
        // Push depth parameters to shader (mode, inversion, normalization)
        // Only push if we have a valid pipeline layout with push constant range
        if (pLogicalSwapchain->depthResolvePipelineLayout != VK_NULL_HANDLE)
        {
            // Struct must match GLSL layout(push_constant) uniform DepthParams exactly:
            //   int  depthMode;    = int32_t (4 bytes)
            //   bool invertDepth; = int32_t (4 bytes in GLSL)
            //   bool normalize;   = int32_t (4 bytes in GLSL)
            // Total: 12 bytes
            struct DepthPushConstants {
                int32_t depthMode;
                int32_t invertDepth;
                int32_t normalize;
            };
            
            static_assert(sizeof(DepthPushConstants) == sizeof(int32_t) * 3, 
                          "DepthPushConstants size must be 12 bytes to match GLSL");
            
            DepthPushConstants pushData = {};
            pushData.depthMode = static_cast<int32_t>(settingsManager.getDepthSourceChannel()); // 0=R, 1=Alpha, etc.
            pushData.invertDepth = settingsManager.getDepthInvert() ? 1 : 0;
            pushData.normalize = 1; // Always normalize for safety
            
            pLogicalDevice->vkd.CmdPushConstants(
                commandBuffer,
                pLogicalSwapchain->depthResolvePipelineLayout,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                0,
                sizeof(DepthPushConstants),
                &pushData
            );
        }
        
        pLogicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);
        pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized = true;

        if (shouldKeepObservedDepthInGeneralLayout(depthState))
            return;

        sourceBarrier.oldLayout = getInternalDepthReadOnlyLayout(depthState.format);
        sourceBarrier.newLayout = getObservedDepthAttachmentLayout(depthState);
        sourceBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceBarrier.dstAccessMask = getObservedDepthRestoreAccess(depthState);
        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               getObservedDepthRestoreStages(depthState),
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &sourceBarrier);
    }

    std::vector<VkCommandBuffer> allocateCommandBuffer(LogicalDevice* pLogicalDevice, uint32_t count)
    {
        std::vector<VkCommandBuffer> commandBuffers(count);

        VkCommandBufferAllocateInfo allocInfo;
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.pNext              = nullptr;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool        = pLogicalDevice->commandPool;
        allocInfo.commandBufferCount = count;

        VkResult result = pLogicalDevice->vkd.AllocateCommandBuffers(pLogicalDevice->device, &allocInfo, commandBuffers.data());
        if (result != VK_SUCCESS)
        {
            Logger::err("ASSERT_VULKAN failed in " + std::string(__FILE__) + " : " + std::to_string(__LINE__) + "; " + std::to_string(result));
            return {};
        }
        for (uint32_t i = 0; i < count; i++)
        {
            // initialize dispatch tables for commandBuffers since the are dispatchable objects
            initializeDispatchTable(commandBuffers[i], pLogicalDevice->device);
        }

        return commandBuffers;
    }
    void writeCommandBuffers(LogicalDevice*                                 pLogicalDevice,
                             LogicalSwapchain*                              pLogicalSwapchain,
                             std::vector<std::shared_ptr<vkShade::Effect>> effects,
                             std::vector<VkCommandBuffer>                   commandBuffers,
                             const DepthState&                              depthState)
    {
        VkCommandBufferBeginInfo beginInfo = {};

        beginInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext            = nullptr;
        beginInfo.flags            = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        beginInfo.pInheritanceInfo = nullptr;

        const bool hasDepthResolveTarget = hasDepthResolveResources(pLogicalSwapchain, commandBuffers.size());

        // Defensive: if the resolve resources exist but their baked source view
        // doesn't match the current depth state (e.g. depth changed but realloc
        // hasn't run yet), treat the resolve target as unusable this frame.
        // Effects will get VK_NULL_HANDLE for the depth image and skip their
        // depth reads rather than sampling a stale/destroyed view.
        const bool depthResolveTargetUsable = hasDepthResolveTarget
            && (!hasDepthState(depthState)
                || pLogicalSwapchain->depthResolveSourceView == VK_NULL_HANDLE
                || pLogicalSwapchain->depthResolveSourceView == depthState.imageView);

        for (uint32_t i = 0; i < commandBuffers.size(); i++)
        {
            const VkImageView  boundDepthImageView = depthResolveTargetUsable ? pLogicalSwapchain->depthResolvePerImage[i].imageView
                                                                               : VK_NULL_HANDLE;
            const VkImageLayout boundDepthLayout   = depthResolveTargetUsable ? getDepthResolveReadOnlyLayout(pLogicalSwapchain)
                                                                               : VK_IMAGE_LAYOUT_UNDEFINED;
            for (auto& effect : effects)
            {
                effect->useDepthImage(i, boundDepthImageView, boundDepthLayout);
            }

            VkResult result = pLogicalDevice->vkd.BeginCommandBuffer(commandBuffers[i], &beginInfo);
            ASSERT_VULKAN(result);

            if (depthResolveTargetUsable && hasDepthState(depthState))
            {
                recordDepthResolveSnapshot(
                    pLogicalDevice, pLogicalSwapchain, commandBuffers[i], i, depthState);
            }

            for (uint32_t j = 0; j < effects.size(); j++)
            {
                effects[j]->applyEffect(i, commandBuffers[i]);
            }

            result = pLogicalDevice->vkd.EndCommandBuffer(commandBuffers[i]);
            ASSERT_VULKAN(result);
        }
    }

    // Resolve a multisampled depth attachment into the 1-sample resolve target
    // via the depth-stencil resolve subpass render pass built in
    // initializeDepthResolveLayout. No draws are recorded; the resolve happens
    // implicitly at subpass end. The resolve target ends in the read-only depth
    // layout so effects can sample it.
    static void recordDepthResolveSnapshotViaMsaaSubpass(LogicalDevice*     pLogicalDevice,
                                                         LogicalSwapchain*  pLogicalSwapchain,
                                                         VkCommandBuffer    commandBuffer,
                                                         uint32_t           imageIndex,
                                                         const DepthState&  depthState)
    {
        if (imageIndex >= pLogicalSwapchain->depthResolveMsaaFramebuffers.size())
            return;

        const bool sourceInGeneral = (depthState.observedLayout == VK_IMAGE_LAYOUT_GENERAL);

        // When the MSAA source is in GENERAL layout, barrier it to
        // DEPTH_STENCIL_ATTACHMENT_OPTIMAL for the resolve subpass (which
        // requires attachment-optimal as its initial/final layout). We restore it
        // to GENERAL after.
        if (sourceInGeneral)
        {
            VkImageMemoryBarrier toAttachment = {};
            toAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toAttachment.image = depthState.image;
            toAttachment.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toAttachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            toAttachment.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            toAttachment.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toAttachment.subresourceRange.aspectMask =
                isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
            toAttachment.subresourceRange.baseMipLevel = 0;
            toAttachment.subresourceRange.levelCount = 1;
            toAttachment.subresourceRange.baseArrayLayer = 0;
            toAttachment.subresourceRange.layerCount = 1;
            pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                                   0, 0, nullptr, 0, nullptr, 1, &toAttachment);
        }

        VkRenderPassBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = pLogicalSwapchain->depthResolveMsaaRenderPass;
        beginInfo.framebuffer = pLogicalSwapchain->depthResolveMsaaFramebuffers[imageIndex];
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderArea.extent = {pLogicalSwapchain->depthResolveExtent.width, pLogicalSwapchain->depthResolveExtent.height};
        beginInfo.clearValueCount = 0;
        beginInfo.pClearValues = nullptr;

        pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        // Restore source layout if it was GENERAL.
        if (sourceInGeneral)
        {
            VkImageMemoryBarrier toGeneral = {};
            toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toGeneral.image = depthState.image;
            toGeneral.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toGeneral.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            toGeneral.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.subresourceRange.aspectMask =
                isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
            toGeneral.subresourceRange.baseMipLevel = 0;
            toGeneral.subresourceRange.levelCount = 1;
            toGeneral.subresourceRange.baseArrayLayer = 0;
            toGeneral.subresourceRange.layerCount = 1;
            pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                   0, 0, nullptr, 0, nullptr, 1, &toGeneral);
        }

        pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized = true;

        Logger::debug("depth MSAA resolve via subpass: image=" + convertToString(depthState.image)
                      + " samples=" + convertToString(depthState.samples)
                      + " mode=" + std::to_string(pLogicalSwapchain->depthResolveMode)
                      + " sourceWasGeneral=" + std::string(sourceInGeneral ? "true" : "false"));
    }

    // MSAA depth resolve FALLBACK via vkCmdResolveImage.
    // This mirrors ReShade's approach: use the plain vkCmdResolveImage command
    // (available since Vulkan 1.0) instead of the depth-stencil resolve subpass
    // (which requires Vulkan 1.2 or VK_KHR_depth_stencil_resolve).
    //
    // vkCmdResolveImage resolves all samples by averaging (for depth) — there's
    // no way to pick a specific sample or mode. For depth effects, averaging is
    // usually fine; the alternative (sample-zero) requires the extension.
    //
    // Layout transitions:
    //   source (MSAA depth):  ATTACHMENT_OPTIMAL → TRANSFER_SRC_OPTIMAL → ATTACHMENT_OPTIMAL
    //   target (1-sample):    read-only → TRANSFER_DST_OPTIMAL → read-only
    static void recordDepthResolveSnapshotViaCmdResolveImage(LogicalDevice*     pLogicalDevice,
                                                             LogicalSwapchain*  pLogicalSwapchain,
                                                             VkCommandBuffer    commandBuffer,
                                                             uint32_t           imageIndex,
                                                             const DepthState&  depthState)
    {
        if (imageIndex >= pLogicalSwapchain->depthResolvePerImage.size())
            return;

        const VkImageLayout observedLayout = getObservedDepthAttachmentLayout(depthState);
        const bool sourceInGeneral = shouldKeepObservedDepthInGeneralLayout(depthState);
        const VkImageAspectFlags aspectMask =
            isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;

        // --- Source barrier: MSAA depth → TRANSFER_SRC_OPTIMAL ---
        VkImageMemoryBarrier srcBarrier = {};
        srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        srcBarrier.image = depthState.image;
        srcBarrier.oldLayout = observedLayout;
        srcBarrier.newLayout = sourceInGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.srcAccessMask = getObservedDepthSourceAccess(depthState);
        srcBarrier.dstAccessMask = sourceInGeneral
            ? (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
            : VK_ACCESS_TRANSFER_READ_BIT;
        srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.subresourceRange.aspectMask = aspectMask;
        srcBarrier.subresourceRange.baseMipLevel = 0;
        srcBarrier.subresourceRange.levelCount = 1;
        srcBarrier.subresourceRange.baseArrayLayer = 0;
        srcBarrier.subresourceRange.layerCount = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               getObservedDepthSourceStages(depthState),
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

        // --- Target barrier: read-only → TRANSFER_DST_OPTIMAL ---
        VkImageMemoryBarrier dstBarrier = {};
        dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        dstBarrier.image = pLogicalSwapchain->depthResolvePerImage[imageIndex].image;
        dstBarrier.oldLayout = pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized
            ? getInternalDepthReadOnlyLayout(pLogicalSwapchain->depthResolveFormat)
            : VK_IMAGE_LAYOUT_UNDEFINED;
        dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.srcAccessMask = pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        dstBarrier.subresourceRange.baseMipLevel = 0;
        dstBarrier.subresourceRange.levelCount = 1;
        dstBarrier.subresourceRange.baseArrayLayer = 0;
        dstBarrier.subresourceRange.layerCount = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized
                                                   ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

        // --- vkCmdResolveImage: MSAA depth → 1-sample resolve target ---
        VkImageResolve resolveRegion = {};
        resolveRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        resolveRegion.srcSubresource.mipLevel = 0;
        resolveRegion.srcSubresource.baseArrayLayer = 0;
        resolveRegion.srcSubresource.layerCount = 1;
        resolveRegion.srcOffset = {0, 0, 0};
        resolveRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        resolveRegion.dstSubresource.mipLevel = 0;
        resolveRegion.dstSubresource.baseArrayLayer = 0;
        resolveRegion.dstSubresource.layerCount = 1;
        resolveRegion.dstOffset = {0, 0, 0};
        resolveRegion.extent = {depthState.extent.width, depthState.extent.height, 1};

        pLogicalDevice->vkd.CmdResolveImage(commandBuffer,
                                            depthState.image,
                                            sourceInGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            pLogicalSwapchain->depthResolvePerImage[imageIndex].image,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            1,
                                            &resolveRegion);

        // --- Target barrier: TRANSFER_DST_OPTIMAL → read-only ---
        dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.newLayout = getInternalDepthReadOnlyLayout(pLogicalSwapchain->depthResolveFormat);
        dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

        pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized = true;

        // --- Restore source layout if we transitioned away from GENERAL ---
        if (!sourceInGeneral)
        {
            srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            srcBarrier.newLayout = observedLayout;
            srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            srcBarrier.dstAccessMask = getObservedDepthRestoreAccess(depthState);
            pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                   getObservedDepthRestoreStages(depthState),
                                                   0, 0, nullptr, 0, nullptr, 1, &srcBarrier);
        }

        Logger::debug("depth MSAA resolve via vkCmdResolveImage: image=" + convertToString(depthState.image)
                      + " samples=" + convertToString(depthState.samples)
                      + " sourceWasGeneral=" + std::string(sourceInGeneral ? "true" : "false"));
    }

    void recordDepthResolveSnapshot(LogicalDevice*  pLogicalDevice,
                                    LogicalSwapchain* pLogicalSwapchain,
                                    VkCommandBuffer commandBuffer,
                                    uint32_t imageIndex,
                                    const DepthState& depthState)
    {
        if (!pLogicalDevice || !pLogicalSwapchain)
            return;
        if (!hasDepthState(depthState))
            return;
        if (depthState.extent.width == 0 || depthState.extent.height == 0)
            return;
        if (imageIndex >= pLogicalSwapchain->depthResolvePerImage.size())
            return;

        // Defensive: ensure the underlying depth image is still tracked and has
        // the usage flags we need. This catches the race where Roblox (or any
        // app) destroys a depth image between the time the resolve resources
        // were built and the time we record this command buffer. Without this
        // check, vkCmdCopyImage / vkCmdResolveImage would operate on a dangling
        // image handle, producing either a device-loss or silent garbage.
        // Note: persistent storage is tracked separately, not in depthImages.
        if (depthState.image != pLogicalDevice->depthCaptureStorage.image
            || !pLogicalDevice->persistentStorageTracked)
        {
            auto imageIt = std::find(pLogicalDevice->depthImages.begin(),
                                     pLogicalDevice->depthImages.end(),
                                     depthState.image);
            if (imageIt == pLogicalDevice->depthImages.end())
            {
                Logger::warn("recordDepthResolveSnapshot: depth image no longer tracked; skipping (image="
                             + convertToString(depthState.image) + ")");
                return;
            }
        } // end persistent-storage bypass
        auto metadataIt = pLogicalDevice->depthImageMetadata.find(depthState.image);
        if (metadataIt != pLogicalDevice->depthImageMetadata.end())
        {
            const VkImageUsageFlags required = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if ((metadataIt->second.usage & required) != required)
            {
                Logger::warn("recordDepthResolveSnapshot: depth image missing required usage flags; skipping (image="
                             + convertToString(depthState.image) + ")");
                return;
            }
        }

        // CRITICAL: if the framebuffer baked into the MSAA resolve path references
        // a different source view than the current depth state, skip the snapshot
        // rather than reading from the wrong image. ensureDepthResolveResources
        // should have already rebuilt the framebuffers, but if for any reason it
        // didn't (race, missed realloc, etc.), this prevents silent corruption.
        if (pLogicalSwapchain->depthResolveIsMsaa
            && pLogicalSwapchain->depthResolveSourceView != VK_NULL_HANDLE
            && pLogicalSwapchain->depthResolveSourceView != depthState.imageView)
        {
            Logger::warn("recordDepthResolveSnapshot: MSAA framebuffer source view mismatch (fb="
                         + convertToString(pLogicalSwapchain->depthResolveSourceView)
                         + " cur=" + convertToString(depthState.imageView)
                         + "); skipping snapshot until realloc catches up");
            return;
        }

        auto logMetadataIt = pLogicalDevice->depthImageMetadata.find(depthState.image);
        if (logMetadataIt != pLogicalDevice->depthImageMetadata.end())
        {
            Logger::debug("depth snapshot source state: commandBuffer=" + convertToString(commandBuffer)
                          + " image=" + convertToString(depthState.image)
                          + " observedLayout=" + convertToString(depthState.observedLayout)
                          + " usage=" + convertToString(logMetadataIt->second.usage)
                          + " samples=" + convertToString(logMetadataIt->second.samples)
                          + " tiling=" + convertToString(logMetadataIt->second.tiling)
                          + " transient=" + std::string((logMetadataIt->second.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 ? "true" : "false"));
        }
        else
        {
            Logger::debug("depth snapshot source state: commandBuffer=" + convertToString(commandBuffer)
                          + " image=" + convertToString(depthState.image)
                          + " observedLayout=" + convertToString(depthState.observedLayout)
                          + " metadata=missing");
        }

        if (pLogicalSwapchain->depthResolveUsesShader)
        {
            recordDepthResolveSnapshotViaShader(pLogicalDevice, pLogicalSwapchain, commandBuffer, imageIndex, depthState);
            return;
        }

        // MSAA depth resolve via a depth-stencil resolve subpass. The render
        // pass has no draws; the implementation resolves attachment 0 (MSAA
        // depth source) into attachment 1 (1-sample resolve target) at subpass
        // end. Effects then sample attachment 1 in the read-only depth layout.
        if (pLogicalSwapchain->depthResolveIsMsaa
            && pLogicalSwapchain->depthResolveMsaaRenderPass != VK_NULL_HANDLE
            && imageIndex < pLogicalSwapchain->depthResolveMsaaFramebuffers.size())
        {
            recordDepthResolveSnapshotViaMsaaSubpass(pLogicalDevice, pLogicalSwapchain, commandBuffer, imageIndex, depthState);
            return;
        }

        // MSAA depth resolve FALLBACK via vkCmdResolveImage.
        // Used when:
        //   - The depth-stencil resolve subpass isn't supported (no
        //     VK_KHR_depth_stencil_resolve, or device doesn't support the
        //     requested resolve mode), OR
        //   - createDepthMsaaResolveRenderPass returned VK_NULL_HANDLE.
        // This is the path ReShade uses for MSAA depth: a plain
        // vkCmdResolveImage with aspectMask=DEPTH. It's supported on every
        // Vulkan device and doesn't require the depth-stencil resolve extension.
        if (pLogicalSwapchain->depthResolveIsMsaa
            && (pLogicalSwapchain->depthResolveMsaaRenderPass == VK_NULL_HANDLE
                || imageIndex >= pLogicalSwapchain->depthResolveMsaaFramebuffers.size()))
        {
            recordDepthResolveSnapshotViaCmdResolveImage(pLogicalDevice, pLogicalSwapchain, commandBuffer, imageIndex, depthState);
            return;
        }

        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext               = nullptr;
        memoryBarrier.image               = depthState.image;
        const bool keepGeneralLayout = shouldKeepObservedDepthInGeneralLayout(depthState);
        memoryBarrier.oldLayout           = getObservedDepthAttachmentLayout(depthState);
        memoryBarrier.newLayout           = keepGeneralLayout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        memoryBarrier.srcAccessMask       = getObservedDepthSourceAccess(depthState);
        memoryBarrier.dstAccessMask       = keepGeneralLayout
                                                ? (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
                                                : VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.subresourceRange.aspectMask =
            isStencilFormat(depthState.format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = 1;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               getObservedDepthSourceStages(depthState),
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &memoryBarrier);

        VkImageMemoryBarrier resolveBarrier = {};
        resolveBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        resolveBarrier.pNext               = nullptr;
        resolveBarrier.image               = pLogicalSwapchain->depthResolvePerImage[imageIndex].image;
        resolveBarrier.oldLayout = pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized
                                       ? getInternalDepthReadOnlyLayout(pLogicalSwapchain->depthResolveFormat)
                                       : VK_IMAGE_LAYOUT_UNDEFINED;
        resolveBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        resolveBarrier.srcAccessMask       = pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        resolveBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        resolveBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        resolveBarrier.subresourceRange.baseMipLevel   = 0;
        resolveBarrier.subresourceRange.levelCount     = 1;
        resolveBarrier.subresourceRange.baseArrayLayer = 0;
        resolveBarrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized
                                                   ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        VkImageCopy copyRegion = {};
        copyRegion.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        copyRegion.srcSubresource.mipLevel       = 0;
        copyRegion.srcSubresource.baseArrayLayer = 0;
        copyRegion.srcSubresource.layerCount     = 1;
        copyRegion.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        copyRegion.dstSubresource.mipLevel       = 0;
        copyRegion.dstSubresource.baseArrayLayer = 0;
        copyRegion.dstSubresource.layerCount     = 1;
        copyRegion.extent.width                  = depthState.extent.width;
        copyRegion.extent.height                 = depthState.extent.height;
        copyRegion.extent.depth                  = 1;

        pLogicalDevice->vkd.CmdCopyImage(commandBuffer,
                                         depthState.image,
                                         keepGeneralLayout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         pLogicalSwapchain->depthResolvePerImage[imageIndex].image,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         1,
                                         &copyRegion);

        resolveBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        resolveBarrier.newLayout     = getInternalDepthReadOnlyLayout(pLogicalSwapchain->depthResolveFormat);
        resolveBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        resolveBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        pLogicalSwapchain->depthResolvePerImage[imageIndex].initialized = true;

        if (!keepGeneralLayout)
        {
            memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            memoryBarrier.newLayout     = getObservedDepthAttachmentLayout(depthState);
            memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            memoryBarrier.dstAccessMask = getObservedDepthRestoreAccess(depthState);
            pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                   getObservedDepthRestoreStages(depthState),
                                                   0,
                                                   0,
                                                   nullptr,
                                                   0,
                                                   nullptr,
                                                   1,
                                                   &memoryBarrier);
        }
    }

    std::vector<VkSemaphore> createSemaphores(LogicalDevice* pLogicalDevice, uint32_t count)
    {
        std::vector<VkSemaphore> semaphores(count);
        VkSemaphoreCreateInfo    info;
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = nullptr;
        info.flags = 0;

        for (uint32_t i = 0; i < count; i++)
        {
            pLogicalDevice->vkd.CreateSemaphore(pLogicalDevice->device, &info, nullptr, &semaphores[i]);
        }
        return semaphores;
    }

} // namespace vkShade
