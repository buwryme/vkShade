#ifndef LOGICAL_DEVICE_HPP_INCLUDED
#define LOGICAL_DEVICE_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>
#include <unordered_map>

#include "vulkan_include.hpp"
#include "vkdispatch.hpp"
#include <mutex>

namespace vkShade
{
    struct LogicalSwapchain;  // Forward declaration

    // Global lock guarding all layer state (defined in vkshade.cpp). Shared with
    // the overlay so it can read LogicalDevice state for the Advanced tab.
    extern std::mutex globalLock;

    struct DepthState
    {
        VkImageView imageView = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent3D extent = {0, 0, 1};
        VkImageLayout observedLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        // Sample count of the source depth image. 1 for non-MSAA, >1 for MSAA
        // depth buffers (e.g. Roblox). The resolve path uses this to decide
        // between copy / resolve / shader-resolve.
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        // True when the source image was created with
        // VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT. Transient images may have
        // restricted lifetime; the layer forces storeOp=STORE and uses the
        // depth-stencil resolve subpass to capture depth before potential
        // discard.
        bool transient = false;
    };

    struct DepthSnapshotTarget
    {
        VkSwapchainKHR  swapchain = VK_NULL_HANDLE;
        LogicalSwapchain* pLogicalSwapchain = nullptr;
        uint32_t        imageIndex = 0;
    };

    // Persistent depth storage image — survives app depth image destruction.
    // Used by the v3 deferred copy mechanism.
    struct PersistentDepthStorage
    {
        VkImage        image = VK_NULL_HANDLE;
        VkImageView    view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkExtent3D     extent = {0, 0, 1};
        VkFormat       format = VK_FORMAT_UNDEFINED;
        bool           valid = false;  // True when depth has been blitted this frame
    };

    struct DepthImageMetadata
    {
        VkImageUsageFlags     usage = 0;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkImageTiling         tiling = VK_IMAGE_TILING_OPTIMAL;
    };

    struct OverlayPersistentState;  // Forward declaration
    class ImGuiOverlay;  // Forward declaration

    struct LogicalDevice
    {
        struct DepthScopeTrackingState
        {
            bool     inRenderScope = false;
            DepthState depthState;
            DepthSnapshotTarget snapshotTarget;
            uint32_t  drawCount = 0;
            // The depth attachment's finalLayout from the render pass description.
            // This is the EXACT layout the depth image will be in after the render
            // pass ends — ground truth, no guessing. For dynamic rendering, this
            // comes from VkRenderingAttachmentInfo::imageLayout.
            VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        };

        struct DepthCandidateTrackingState
        {
            bool     valid = false;
            DepthState depthState;
            bool     hasPresentableSnapshotTarget = false;
            bool     extentMatchesPresentableTarget = false;
            uint32_t  drawCount = 0;
        };

        DeviceDispatch           vkd;
        InstanceDispatch         vki;
        VkDevice                 device;
        VkPhysicalDevice         physicalDevice;
        VkInstance               instance;
        VkQueue                  queue;
        uint32_t                 queueFamilyIndex;
        VkCommandPool            commandPool;
        bool                     supportsMutableFormat;
        bool                     isNvidiaGpu;
        bool                     gpuCrashDiagnosticsEnabled = false;
        bool                     supportsNvDiagnosticCheckpoints = false;
        bool                     supportsNvDiagnosticsConfig = false;
        bool                     supportsDeviceFaultExt = false;
        // Queried once at device init via VK_KHR_depth_resolve_mode / core 1.2.
        // OR of VK_RESOLVE_MODE_SAMPLE_ZERO_BIT / AVERAGE_BIT / MIN/MAX. Effects
        // the MSAA depth resolve path and the Advanced UI mode selector.
        VkResolveModeFlags       supportedDepthResolveModes = 0;
        // Optional manual override of the depth buffer promotion. When set to
        // a known tracked image view, the layer pins to it instead of using the
        // best-candidate heuristic. Set from the Advanced UI (empty = auto).
        VkImageView              pinnedDepthImageView = VK_NULL_HANDLE;
        std::vector<VkImage>     depthImages;
        std::vector<VkFormat>    depthFormats;
        std::vector<VkImageView> depthImageViews;
        std::unordered_map<VkImage, VkExtent3D> depthImageExtents;
        std::unordered_map<VkImage, DepthImageMetadata> depthImageMetadata;
        std::unordered_map<VkImageView, DepthState> depthViewStates;
        std::unordered_map<VkImageView, DepthSnapshotTarget> snapshotTargetViewStates;
        std::unordered_map<VkFramebuffer, DepthState> framebufferDepthStates;
        std::unordered_map<VkFramebuffer, DepthSnapshotTarget> framebufferSnapshotTargets;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState> commandBufferDepthStates;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState> pendingTransferLinkedDepthScopes;
        std::unordered_map<VkCommandBuffer, uint32_t> commandBufferRecordedDrawCounts;
        DepthCandidateTrackingState  bestDepthCandidate;
        DepthState               activeDepthState;

        // When a depth candidate is promoted during CmdEndRenderPass, we MUST NOT
        // free/reallocate effect command buffers immediately — the GPU may still
        // be executing the old ones submitted by the previous QueuePresentKHR.
        // Instead, set this flag and handle the reallocation in QueuePresentKHR
        // where we can safely QueueWaitIdle first.
        bool depthReallocPending = false;

        // Persistent depth storage — layer-internal depth image that outlives
        // app depth image destruction. NOT stored in the parallel
        // depthImages/depthFormats/depthImageViews vectors.
        PersistentDepthStorage depthCaptureStorage;
        bool persistentStorageTracked = false;

        // v3 deferred copy: depth that needs blitting at QueueSubmit time.
        struct
        {
            DepthState     depthState;
            VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bool           pending = false;
        } pendingDepthCopy;

        // Ring buffer of pre-allocated command buffers for depth copy.
        // Size 4 supports up to 4 frames in flight. Uses a dedicated
        // transient command pool (separate from the main effect pool).
        //
        // CRITICAL: Each slot has a companion fence that is signaled when the
        // command buffer is submitted.  Before reusing a slot we MUST wait on
        // its fence — otherwise we reset/re-record a CB the GPU may still be
        // executing, which causes VK_ERROR_DEVICE_LOST (the exact symptom
        // reported by users).
        static constexpr uint32_t DEPTH_COPY_RING_SIZE = 4;
        VkCommandPool            depthCopyPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> depthCopyRingBufs;
        std::vector<VkFence>      depthCopyRingFences;  // One per slot, signaled on submit
        uint32_t                 depthCopyRingIndex = 0;

        // Persistent overlay state that survives swapchain recreation
        std::unique_ptr<OverlayPersistentState> overlayPersistentState;

        // ImGui overlay - lives at device level to survive swapchain recreation
        std::unique_ptr<ImGuiOverlay> imguiOverlay;
    };
} // namespace vkShade

#endif // LOGICAL_DEVICE_HPP_INCLUDED