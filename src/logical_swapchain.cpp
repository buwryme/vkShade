#include "logical_swapchain.hpp"

namespace VKIntox
{
    void destroyDepthResolveResources(LogicalSwapchain* pLogicalSwapchain);

    void LogicalSwapchain::destroy()
    {
        if (imageCount > 0)
        {
            // Wait for GPU to finish before destroying resources
            Logger::info("[DESTROY-TRACE] LogicalSwapchain::destroy: QueueWaitIdle");
            pLogicalDevice->vkd.QueueWaitIdle(pLogicalDevice->queue);

            // Reset the command pool BEFORE destroying effects.  This puts all
            // allocated command buffers back into the initial state, clearing
            // any driver-internal tracking of which pipelines / render passes
            // they reference.  Some NVIDIA driver versions crash if effect
            // objects are destroyed while the driver still has tracking records.
            Logger::info("[DESTROY-TRACE] ResetCommandPool");
            pLogicalDevice->vkd.ResetCommandPool(pLogicalDevice->device, pLogicalDevice->commandPool, 0);

            // Free command buffers
            if (!commandBuffersEffect.empty())
            {
                pLogicalDevice->vkd.FreeCommandBuffers(
                    pLogicalDevice->device, pLogicalDevice->commandPool, commandBuffersEffect.size(), commandBuffersEffect.data());
                commandBuffersEffect.clear();
            }
            if (!commandBuffersNoEffect.empty())
            {
                pLogicalDevice->vkd.FreeCommandBuffers(
                    pLogicalDevice->device, pLogicalDevice->commandPool, commandBuffersNoEffect.size(), commandBuffersNoEffect.data());
                commandBuffersNoEffect.clear();
            }
            Logger::info("[DESTROY-TRACE] command buffers freed");

            Logger::info("[DESTROY-TRACE] destroying " + std::to_string(effects.size()) + " effects");
            effects.clear();
            defaultTransfer.reset();
            Logger::info("[DESTROY-TRACE] effects destroyed");

            destroyDepthResolveResources(this);

            for (VkDeviceMemory mem : fakeImageMemories)
                pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, mem, nullptr);
            fakeImageMemories.clear();

            for (uint32_t i = 0; i < fakeImages.size(); i++)
            {
                pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, fakeImages[i], nullptr);
            }

            for (unsigned int i = 0; i < imageCount; i++)
            {
                pLogicalDevice->vkd.DestroySemaphore(pLogicalDevice->device, semaphores[i], nullptr);
                pLogicalDevice->vkd.DestroySemaphore(pLogicalDevice->device, overlaySemaphores[i], nullptr);
            }

            // Destroy per-image effect submit fences
            for (VkFence f : effectSubmitFences)
            {
                if (f != VK_NULL_HANDLE)
                    pLogicalDevice->vkd.DestroyFence(pLogicalDevice->device, f, nullptr);
            }
            effectSubmitFences.clear();

            Logger::debug("after DestroySemaphore/Fence");

            // Destroy image views for overlay
            for (auto& view : imageViews)
            {
                pLogicalDevice->vkd.DestroyImageView(pLogicalDevice->device, view, nullptr);
            }
            imageViews.clear();

            // Note: ImGui overlay is now at device level, not destroyed here
        }
    }
} // namespace VKIntox
