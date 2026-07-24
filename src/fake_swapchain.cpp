#include "fake_swapchain.hpp"
#include "memory.hpp"
#include "format.hpp"

#include <limits>

namespace VKIntox
{
    std::vector<VkImage> createFakeSwapchainImages(LogicalDevice*               pLogicalDevice,
                                                   VkSwapchainCreateInfoKHR     swapchainCreateInfo,
                                                   uint32_t                    count,
                                                   std::vector<VkDeviceMemory>& deviceMemories)
    {
        deviceMemories.clear();
        if (count == 0)
        {
            Logger::err("Cannot create fake swapchain images with count=0");
            return {};
        }

        std::vector<VkImage> fakeImages(count);

        VkFormat srgbFormat =
            isSRGB(swapchainCreateInfo.imageFormat) ? swapchainCreateInfo.imageFormat : convertToSRGB(swapchainCreateInfo.imageFormat);
        VkFormat unormFormat =
            isSRGB(swapchainCreateInfo.imageFormat) ? convertToUNORM(swapchainCreateInfo.imageFormat) : swapchainCreateInfo.imageFormat;

        VkFormat formats[] = {unormFormat, srgbFormat};

        VkImageFormatListCreateInfoKHR imageFormatListCreateInfo;
        imageFormatListCreateInfo.sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR;
        imageFormatListCreateInfo.pNext           = nullptr;
        imageFormatListCreateInfo.viewFormatCount = 2;
        imageFormatListCreateInfo.pViewFormats    = formats;

        VkImageCreateInfo imageCreateInfo;
        imageCreateInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageCreateInfo.pNext         = (unormFormat == srgbFormat) ? nullptr : &imageFormatListCreateInfo;
        imageCreateInfo.flags         = (unormFormat == srgbFormat) ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        imageCreateInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageCreateInfo.format        = swapchainCreateInfo.imageFormat;
        imageCreateInfo.extent.width  = swapchainCreateInfo.imageExtent.width;
        imageCreateInfo.extent.height = swapchainCreateInfo.imageExtent.height;
        imageCreateInfo.extent.depth  = 1;
        imageCreateInfo.mipLevels     = 1;
        imageCreateInfo.arrayLayers   = swapchainCreateInfo.imageArrayLayers;
        imageCreateInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageCreateInfo.usage         = swapchainCreateInfo.imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageCreateInfo.sharingMode           = swapchainCreateInfo.imageSharingMode;
        imageCreateInfo.queueFamilyIndexCount = swapchainCreateInfo.queueFamilyIndexCount;
        imageCreateInfo.pQueueFamilyIndices   = swapchainCreateInfo.pQueueFamilyIndices;
        imageCreateInfo.initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult result;
        for (uint32_t i = 0; i < count; i++)
        {
            result = pLogicalDevice->vkd.CreateImage(pLogicalDevice->device, &imageCreateInfo, nullptr, &(fakeImages[i]));
            if (result != VK_SUCCESS)
            {
                Logger::err("createFakeSwapchainImages: CreateImage[" + std::to_string(i) + "] failed: " + std::to_string(result));
                for (VkImage img : fakeImages)
                    if (img != VK_NULL_HANDLE)
                        pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, img, nullptr);
                return {};
            }
        }

        // Get memory requirements from first image
        VkMemoryRequirements memoryRequirements;
        pLogicalDevice->vkd.GetImageMemoryRequirements(pLogicalDevice->device, fakeImages[0], &memoryRequirements);

        Logger::debug("fake image size: " + std::to_string(memoryRequirements.size));
        Logger::debug("fake image alignment: " + std::to_string(memoryRequirements.alignment));

        auto memoryTypeIndex = findMemoryTypeIndex(pLogicalDevice, memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryTypeIndex == 0xFFFFFFFF)
        {
            Logger::err("createFakeSwapchainImages: no valid memory type");
            for (VkImage img : fakeImages)
                pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, img, nullptr);
            return {};
        }

        // Pad per-image size to alignment
        VkDeviceSize alignedSize = memoryRequirements.size;
        if (alignedSize % memoryRequirements.alignment != 0)
            alignedSize = (alignedSize / memoryRequirements.alignment + 1) * memoryRequirements.alignment;

        VkDeviceSize bulkSize = alignedSize * count;

        // --- Attempt 1: single bulk allocation ---
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType            = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize   = bulkSize;
        allocInfo.memoryTypeIndex  = memoryTypeIndex;

        VkDeviceMemory bulkMemory = VK_NULL_HANDLE;
        result = pLogicalDevice->vkd.AllocateMemory(pLogicalDevice->device, &allocInfo, nullptr, &bulkMemory);

        if (result == VK_SUCCESS)
        {
            Logger::debug("createFakeSwapchainImages: bulk allocation succeeded (" + std::to_string(bulkSize) + " bytes)");

            bool bindOk = true;
            for (uint32_t i = 0; i < count; i++)
            {
                result = pLogicalDevice->vkd.BindImageMemory(pLogicalDevice->device, fakeImages[i], bulkMemory, alignedSize * i);
                if (result != VK_SUCCESS)
                {
                    Logger::err("createFakeSwapchainImages: BindImageMemory[" + std::to_string(i) + "] failed: " + std::to_string(result));
                    bindOk = false;
                    break;
                }
            }

            if (bindOk)
            {
                deviceMemories.push_back(bulkMemory);
                return fakeImages;
            }

            pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, bulkMemory, nullptr);
        }
        else
        {
            Logger::warn("createFakeSwapchainImages: bulk allocation failed (" + std::to_string(result)
                         + " size=" + std::to_string(bulkSize) + " type=" + std::to_string(memoryTypeIndex)
                         + "), falling back to per-image allocation");
        }

        // --- Attempt 2: per-image allocation ---
        // Handles translation layers (e.g. Sober/Android-on-Linux) that reject
        // large contiguous allocations or report huge alignment requirements.
        allocInfo.allocationSize = memoryRequirements.size; // raw per-image size

        deviceMemories.reserve(count);
        for (uint32_t i = 0; i < count; i++)
        {
            VkDeviceMemory imgMem = VK_NULL_HANDLE;
            result = pLogicalDevice->vkd.AllocateMemory(pLogicalDevice->device, &allocInfo, nullptr, &imgMem);
            if (result != VK_SUCCESS)
            {
                Logger::err("createFakeSwapchainImages: per-image AllocateMemory[" + std::to_string(i)
                             + "] failed: " + std::to_string(result));
                // Free everything allocated so far
                for (VkDeviceMemory m : deviceMemories)
                    pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, m, nullptr);
                for (VkImage img : fakeImages)
                    pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, img, nullptr);
                return {};
            }

            result = pLogicalDevice->vkd.BindImageMemory(pLogicalDevice->device, fakeImages[i], imgMem, 0);
            if (result != VK_SUCCESS)
            {
                Logger::err("createFakeSwapchainImages: per-image BindImageMemory[" + std::to_string(i)
                             + "] failed: " + std::to_string(result));
                pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, imgMem, nullptr);
                for (VkDeviceMemory m : deviceMemories)
                    pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, m, nullptr);
                for (VkImage img : fakeImages)
                    pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, img, nullptr);
                return {};
            }

            deviceMemories.push_back(imgMem);
        }

        Logger::info("createFakeSwapchainImages: per-image allocation succeeded (" + std::to_string(count) + " images)");
        return fakeImages;
    }
} // namespace VKIntox
