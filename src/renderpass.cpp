#include "renderpass.hpp"

namespace vkShade
{
    VkRenderPass createRenderPass(LogicalDevice* pLogicalDevice, VkFormat format, VkImageLayout finalLayout)
    {
        VkRenderPass renderPass;

        VkAttachmentDescription attachmentDescription;
        attachmentDescription.flags          = 0;
        attachmentDescription.format         = format;
        attachmentDescription.samples        = VK_SAMPLE_COUNT_1_BIT;
        attachmentDescription.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachmentDescription.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachmentDescription.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachmentDescription.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachmentDescription.finalLayout    = finalLayout;

        VkAttachmentReference attachmentReference;
        attachmentReference.attachment = 0;
        attachmentReference.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpassDescription;
        subpassDescription.flags                   = 0;
        subpassDescription.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDescription.inputAttachmentCount    = 0;
        subpassDescription.pInputAttachments       = nullptr;
        subpassDescription.colorAttachmentCount    = 1;
        subpassDescription.pColorAttachments       = &attachmentReference;
        subpassDescription.pResolveAttachments     = nullptr;
        subpassDescription.pDepthStencilAttachment = nullptr;
        subpassDescription.preserveAttachmentCount = 0;
        subpassDescription.pPreserveAttachments    = nullptr;

        VkSubpassDependency subpassDependencies[2];

        // Incoming: wait for prior color writes before this render pass
        subpassDependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[0].dstSubpass      = 0;
        subpassDependencies[0].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependencies[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependencies[0].srcAccessMask   = 0;
        subpassDependencies[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        subpassDependencies[0].dependencyFlags = 0;

        // Outgoing: ensure writes + layout transition are visible before subsequent reads
        subpassDependencies[1].srcSubpass      = 0;
        subpassDependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependencies[1].dstStageMask    = (finalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                                      ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                                      : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        subpassDependencies[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        subpassDependencies[1].dstAccessMask   = (finalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                                      ? VK_ACCESS_SHADER_READ_BIT
                                                      : VK_ACCESS_MEMORY_READ_BIT;
        subpassDependencies[1].dependencyFlags = 0;

        VkRenderPassCreateInfo renderPassCreateInfo;
        renderPassCreateInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassCreateInfo.pNext           = nullptr;
        renderPassCreateInfo.flags           = 0;
        renderPassCreateInfo.attachmentCount = 1;
        renderPassCreateInfo.pAttachments    = &attachmentDescription;
        renderPassCreateInfo.subpassCount    = 1;
        renderPassCreateInfo.pSubpasses      = &subpassDescription;
        renderPassCreateInfo.dependencyCount = 2;
        renderPassCreateInfo.pDependencies   = subpassDependencies;

        VkResult result = pLogicalDevice->vkd.CreateRenderPass(pLogicalDevice->device, &renderPassCreateInfo, nullptr, &renderPass);
        ASSERT_VULKAN_VAL(result, VK_NULL_HANDLE);

        return renderPass;
    }

    VkRenderPass createDepthMsaaResolveRenderPass(LogicalDevice*         pLogicalDevice,
                                                  VkFormat               depthFormat,
                                                  VkSampleCountFlagBits  samples,
                                                  VkResolveModeFlagBits  depthResolveMode,
                                                  VkImageLayout          resolveFinalLayout)
    {
        if (samples == VK_SAMPLE_COUNT_1_BIT || depthResolveMode == 0)
            return VK_NULL_HANDLE;

        // Attachment 0: multisampled depth source (input). Loaded, discarded
        // after (the game owns its contents).
        VkAttachmentDescription2 attachments[2];
        attachments[0].sType          = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachments[0].pNext          = nullptr;
        attachments[0].flags          = 0;
        attachments[0].format         = depthFormat;
        attachments[0].samples        = samples;
        attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        // Attachment 1: 1-sample resolve target that effects will sample.
        attachments[1].sType          = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachments[1].pNext          = nullptr;
        attachments[1].flags          = 0;
        attachments[1].format         = depthFormat;
        attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout    = resolveFinalLayout;

        VkAttachmentReference2 depthInputRef;
        depthInputRef.sType       = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depthInputRef.pNext       = nullptr;
        depthInputRef.attachment  = 0;
        depthInputRef.layout      = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthInputRef.aspectMask  = VK_IMAGE_ASPECT_DEPTH_BIT;

        VkAttachmentReference2 depthResolveRef;
        depthResolveRef.sType      = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depthResolveRef.pNext      = nullptr;
        depthResolveRef.attachment = 1;
        depthResolveRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthResolveRef.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

        VkSubpassDescriptionDepthStencilResolve depthResolve;
        depthResolve.sType            = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
        depthResolve.pNext            = nullptr;
        depthResolve.depthResolveMode = depthResolveMode;
        depthResolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
        depthResolve.pDepthStencilResolveAttachment = &depthResolveRef;

        VkSubpassDescription2 subpassDescription;
        subpassDescription.sType                   = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        subpassDescription.pNext                   = &depthResolve;
        subpassDescription.flags                   = 0;
        subpassDescription.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDescription.viewMask                = 0;
        subpassDescription.inputAttachmentCount    = 0;
        subpassDescription.pInputAttachments       = nullptr;
        subpassDescription.colorAttachmentCount    = 0;
        subpassDescription.pColorAttachments       = nullptr;
        subpassDescription.pResolveAttachments     = nullptr;
        subpassDescription.pDepthStencilAttachment = &depthInputRef;
        subpassDescription.preserveAttachmentCount = 0;
        subpassDescription.pPreserveAttachments    = nullptr;

        VkSubpassDependency2 subpassDependencies[2];

        // Wait for the game's depth writes to complete before resolving.
        subpassDependencies[0].sType           = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        subpassDependencies[0].pNext           = nullptr;
        subpassDependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[0].dstSubpass      = 0;
        subpassDependencies[0].srcStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        subpassDependencies[0].dstStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        subpassDependencies[0].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        subpassDependencies[0].dstAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        subpassDependencies[0].dependencyFlags = 0;
        subpassDependencies[0].viewOffset      = 0;

        // Make the resolved depth visible to effects (shader read).
        subpassDependencies[1].sType           = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        subpassDependencies[1].pNext           = nullptr;
        subpassDependencies[1].srcSubpass      = 0;
        subpassDependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[1].srcStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        subpassDependencies[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        subpassDependencies[1].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        subpassDependencies[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        subpassDependencies[1].dependencyFlags = 0;
        subpassDependencies[1].viewOffset      = 0;

        VkRenderPassCreateInfo2 renderPassCreateInfo;
        renderPassCreateInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
        renderPassCreateInfo.pNext           = nullptr;
        renderPassCreateInfo.flags           = 0;
        renderPassCreateInfo.attachmentCount = 2;
        renderPassCreateInfo.pAttachments    = attachments;
        renderPassCreateInfo.subpassCount    = 1;
        renderPassCreateInfo.pSubpasses      = &subpassDescription;
        renderPassCreateInfo.dependencyCount = 2;
        renderPassCreateInfo.pDependencies   = subpassDependencies;
        renderPassCreateInfo.correlatedViewMaskCount = 0;
        renderPassCreateInfo.pCorrelatedViewMasks    = nullptr;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkResult result = pLogicalDevice->vkd.CreateRenderPass2(pLogicalDevice->device, &renderPassCreateInfo, nullptr, &renderPass);
        ASSERT_VULKAN_VAL(result, VK_NULL_HANDLE);

        return renderPass;
    }
} // namespace vkShade
