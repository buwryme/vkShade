#include "imgui_overlay.hpp"
#include "effects/effect_registry.hpp"
#include "settings_manager.hpp"
#include "reshade_parser.hpp"
#include "logger.hpp"
#include "mouse_input.hpp"
#include "keyboard_input.hpp"
#include "input_blocker.hpp"
#include "config_serializer.hpp"
#include "wayland_display.hpp"
#include "wayland_pointer_constraints.hpp"
#include "wayland_input_common.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_vulkan.h"

namespace vkShade
{
    // No-op dummy for Vulkan functions ImGui requests but vkShade doesn't intercept.
    // ImGui's LoadFunctions treats nullptr returns as failures, so we need a valid pointer.
    static void VKAPI_CALL dummyVulkanFunc() {}

    // Function loader using vkShade's dispatch tables
    static PFN_vkVoidFunction imguiVulkanLoaderDummy(const char* function_name, void* user_data)
    {
        LogicalDevice* device = static_cast<LogicalDevice*>(user_data);

        // Device functions from vkShade's dispatch table
        #define CHECK_FUNC(name) if (strcmp(function_name, "vk" #name) == 0) return (PFN_vkVoidFunction)device->vkd.name

        CHECK_FUNC(AllocateCommandBuffers);
        CHECK_FUNC(AllocateDescriptorSets);
        CHECK_FUNC(AllocateMemory);
        CHECK_FUNC(BeginCommandBuffer);
        CHECK_FUNC(BindBufferMemory);
        CHECK_FUNC(BindImageMemory);
        CHECK_FUNC(CmdBeginRenderPass);
        CHECK_FUNC(CmdBindDescriptorSets);
        CHECK_FUNC(CmdBindIndexBuffer);
        CHECK_FUNC(CmdBindPipeline);
        CHECK_FUNC(CmdBindVertexBuffers);
        CHECK_FUNC(CmdCopyBufferToImage);
        CHECK_FUNC(CmdDrawIndexed);
        CHECK_FUNC(CmdEndRenderPass);
        CHECK_FUNC(CmdPipelineBarrier);
        CHECK_FUNC(CmdPushConstants);
        CHECK_FUNC(CmdSetScissor);
        CHECK_FUNC(CmdSetViewport);
        CHECK_FUNC(CreateBuffer);
        CHECK_FUNC(CreateCommandPool);
        CHECK_FUNC(CreateDescriptorPool);
        CHECK_FUNC(CreateDescriptorSetLayout);
        CHECK_FUNC(CreateFence);
        CHECK_FUNC(CreateFramebuffer);
        CHECK_FUNC(CreateGraphicsPipelines);
        CHECK_FUNC(CreateImage);
        CHECK_FUNC(CreateImageView);
        CHECK_FUNC(CreatePipelineLayout);
        CHECK_FUNC(CreateRenderPass);
        CHECK_FUNC(CreateSampler);
        CHECK_FUNC(CreateSemaphore);
        CHECK_FUNC(CreateShaderModule);
        CHECK_FUNC(CreateSwapchainKHR);
        CHECK_FUNC(DestroyBuffer);
        CHECK_FUNC(DestroyCommandPool);
        CHECK_FUNC(DestroyDescriptorPool);
        CHECK_FUNC(DestroyDescriptorSetLayout);
        CHECK_FUNC(DestroyFence);
        CHECK_FUNC(DestroyFramebuffer);
        CHECK_FUNC(DestroyImage);
        CHECK_FUNC(DestroyImageView);
        CHECK_FUNC(DestroyPipeline);
        CHECK_FUNC(DestroyPipelineLayout);
        CHECK_FUNC(DestroyRenderPass);
        CHECK_FUNC(DestroySampler);
        CHECK_FUNC(DestroySemaphore);
        CHECK_FUNC(DestroyShaderModule);
        CHECK_FUNC(DestroySwapchainKHR);
        CHECK_FUNC(EndCommandBuffer);
        CHECK_FUNC(FlushMappedMemoryRanges);
        CHECK_FUNC(FreeCommandBuffers);
        CHECK_FUNC(FreeDescriptorSets);
        CHECK_FUNC(FreeMemory);
        CHECK_FUNC(GetBufferMemoryRequirements);
        CHECK_FUNC(GetDeviceQueue);
        CHECK_FUNC(GetImageMemoryRequirements);
        CHECK_FUNC(GetSwapchainImagesKHR);
        CHECK_FUNC(MapMemory);
        CHECK_FUNC(QueueSubmit);
        CHECK_FUNC(QueueWaitIdle);
        CHECK_FUNC(ResetCommandPool);
        CHECK_FUNC(ResetFences);
        CHECK_FUNC(UnmapMemory);
        CHECK_FUNC(UpdateDescriptorSets);
        CHECK_FUNC(WaitForFences);

        #undef CHECK_FUNC

        // Instance functions from vkShade's dispatch
        #define CHECK_IFUNC(name) if (strcmp(function_name, "vk" #name) == 0) return (PFN_vkVoidFunction)device->vki.name
        CHECK_IFUNC(GetPhysicalDeviceMemoryProperties);
        CHECK_IFUNC(GetPhysicalDeviceProperties);
        CHECK_IFUNC(GetPhysicalDeviceQueueFamilyProperties);
        #undef CHECK_IFUNC

        // Return a no-op dummy for unknown functions. ImGui's LoadFunctions treats
        // nullptr as a load failure, so we must return a valid function pointer.
        // These functions are never actually called by ImGui in our usage.
        return (PFN_vkVoidFunction)dummyVulkanFunc;
    }

    ImGuiOverlay::ImGuiOverlay(LogicalDevice* device, VkFormat swapchainFormat, uint32_t imageCount, OverlayPersistentState* persistentState)
        : pLogicalDevice(device), pPersistentState(persistentState)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigWindowsMoveFromTitleBarOnly = true;

        std::string iniPath = ConfigSerializer::getBaseConfigDir() + "/imgui.ini";
        std::ifstream iniFile(iniPath);
        std::string iniContent((std::istreambuf_iterator<char>(iniFile)),
                                std::istreambuf_iterator<char>());

        if (!iniContent.empty())
            ImGui::LoadIniSettingsFromDisk(iniPath.c_str());

        // ── Material Design 3 inspired styling ──────────────────────────
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();

        // MD3 colour tokens (dark theme, tonal surface)
        ImVec4 md3Surface        = ImVec4(0.12f, 0.12f, 0.16f, 0.82f);
        ImVec4 md3SurfaceDim     = ImVec4(0.08f, 0.08f, 0.10f, 0.90f);
        ImVec4 md3Primary        = ImVec4(0.62f, 0.36f, 1.00f, 1.00f);  // purple accent
        ImVec4 md3OnSurface      = ImVec4(0.92f, 0.90f, 0.96f, 1.00f);
        ImVec4 md3OnSurfaceVar   = ImVec4(0.72f, 0.70f, 0.76f, 1.00f);
        ImVec4 md3Outline        = ImVec4(0.40f, 0.38f, 0.44f, 0.60f);
        ImVec4 md3OutlineVariant = ImVec4(0.28f, 0.26f, 0.32f, 0.50f);

        style.Alpha               = 0.92f;
        style.WindowPadding       = ImVec2(16, 14);
        style.FramePadding        = ImVec2(12, 7);
        style.ItemSpacing         = ImVec2(8, 6);
        style.ItemInnerSpacing    = ImVec2(6, 4);
        style.IndentSpacing       = 20.0f;
        style.ScrollbarSize       = 10.0f;
        style.GrabMinSize         = 10.0f;

        // MD3 roundness (12 dp for cards, 28 dp for FABs, 16 dp for dialogs)
        style.WindowRounding      = 16.0f;
        style.ChildRounding       = 12.0f;
        style.FrameRounding       = 12.0f;
        style.PopupRounding       = 12.0f;
        style.ScrollbarRounding   = 6.0f;
        style.GrabRounding        = 8.0f;
        style.TabRounding         = 12.0f;

        // MD3 border
        style.WindowBorderSize   = 0.0f;
        style.ChildBorderSize    = 0.0f;
        style.PopupBorderSize    = 0.0f;
        style.FrameBorderSize    = 0.0f;
        style.TabBorderSize      = 0.0f;

        // Colours
        style.Colors[ImGuiCol_WindowBg]           = md3Surface;
        style.Colors[ImGuiCol_ChildBg]            = md3SurfaceDim;
        style.Colors[ImGuiCol_PopupBg]            = md3SurfaceDim;
        style.Colors[ImGuiCol_Border]             = md3OutlineVariant;
        style.Colors[ImGuiCol_FrameBg]            = ImVec4(0.16f, 0.16f, 0.20f, 0.60f);
        style.Colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.22f, 0.22f, 0.28f, 0.70f);
        style.Colors[ImGuiCol_FrameBgActive]      = ImVec4(0.26f, 0.26f, 0.32f, 0.80f);
        style.Colors[ImGuiCol_TitleBg]            = md3Surface;
        style.Colors[ImGuiCol_TitleBgActive]      = md3Surface;
        style.Colors[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.10f, 0.10f, 0.12f, 0.60f);
        style.Colors[ImGuiCol_MenuBarBg]          = ImVec4(0.14f, 0.14f, 0.18f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.06f, 0.06f, 0.08f, 0.40f);
        style.Colors[ImGuiCol_ScrollbarGrab]      = md3Outline;
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = md3OnSurfaceVar;
        style.Colors[ImGuiCol_ScrollbarGrabActive]  = md3Primary;
        style.Colors[ImGuiCol_CheckMark]          = md3Primary;
        style.Colors[ImGuiCol_SliderGrab]         = md3Primary;
        style.Colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.72f, 0.48f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_Button]             = ImVec4(0.20f, 0.20f, 0.26f, 0.80f);
        style.Colors[ImGuiCol_ButtonHovered]      = ImVec4(0.28f, 0.28f, 0.36f, 0.85f);
        style.Colors[ImGuiCol_ButtonActive]       = md3Primary;
        style.Colors[ImGuiCol_Header]             = ImVec4(0.20f, 0.20f, 0.26f, 0.70f);
        style.Colors[ImGuiCol_HeaderHovered]      = ImVec4(0.26f, 0.26f, 0.34f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive]       = ImVec4(0.30f, 0.30f, 0.38f, 0.90f);
        style.Colors[ImGuiCol_Separator]          = md3OutlineVariant;
        style.Colors[ImGuiCol_SeparatorHovered]   = md3OnSurfaceVar;
        style.Colors[ImGuiCol_SeparatorActive]    = md3Primary;
        style.Colors[ImGuiCol_ResizeGrip]         = ImVec4(0.26f, 0.26f, 0.34f, 0.30f);
        style.Colors[ImGuiCol_ResizeGripHovered]  = ImVec4(0.36f, 0.36f, 0.46f, 0.60f);
        style.Colors[ImGuiCol_ResizeGripActive]   = md3Primary;
        style.Colors[ImGuiCol_Tab]                = ImVec4(0.16f, 0.16f, 0.20f, 0.80f);
        style.Colors[ImGuiCol_TabHovered]         = ImVec4(0.24f, 0.24f, 0.30f, 0.90f);
        style.Colors[ImGuiCol_TabSelected]        = ImVec4(0.22f, 0.22f, 0.28f, 1.00f);
        style.Colors[ImGuiCol_TabDimmed]          = ImVec4(0.12f, 0.12f, 0.16f, 0.60f);
        style.Colors[ImGuiCol_TabDimmedSelected]   = ImVec4(0.18f, 0.18f, 0.22f, 0.90f);
        style.Colors[ImGuiCol_DockingPreview]     = md3Primary;
        style.Colors[ImGuiCol_DockingEmptyBg]     = ImVec4(0.08f, 0.08f, 0.10f, 0.60f);
        style.Colors[ImGuiCol_TextSelectedBg]     = ImVec4(0.62f, 0.36f, 1.00f, 0.35f);
        style.Colors[ImGuiCol_NavWindowingHighlight] = md3Primary;
        style.Colors[ImGuiCol_Text]               = md3OnSurface;
        style.Colors[ImGuiCol_TextDisabled]       = ImVec4(0.38f, 0.38f, 0.42f, 1.00f);

        // ── Font: Google Sans Flex with fallback chain ─────────────────
        // Priority: bundled → system paths → ImGui default
        static const char* fontSearchPaths[] = {
            "/usr/share/fonts/truetype/google-sans-flex/GoogleSansFlex-Regular.ttf",
            "/usr/share/fonts/google-sans-flex/GoogleSansFlex-Regular.ttf",
            "/usr/local/share/fonts/google-sans-flex/GoogleSansFlex-Regular.ttf",
            "/usr/share/fonts/TTF/GoogleSansFlex-Regular.ttf",
            "~/.local/share/fonts/GoogleSansFlex-Regular.ttf",
            "GoogleSansFlex-Regular.ttf",   // cwd / flatpak sandbox
        };
        static const char* fontBoldPaths[] = {
            "/usr/share/fonts/truetype/google-sans-flex/GoogleSansFlex-Medium.ttf",
            "/usr/share/fonts/google-sans-flex/GoogleSansFlex-Medium.ttf",
            "/usr/local/share/fonts/google-sans-flex/GoogleSansFlex-Medium.ttf",
            "/usr/share/fonts/TTF/GoogleSansFlex-Medium.ttf",
            "~/.local/share/fonts/GoogleSansFlex-Medium.ttf",
            "GoogleSansFlex-Medium.ttf",
        };

        const char* regularPath = nullptr;
        const char* boldPath   = nullptr;
        for (const char* p : fontSearchPaths)
        {
            if (std::ifstream(p).good()) { regularPath = p; break; }
        }
        for (const char* p : fontBoldPaths)
        {
            if (std::ifstream(p).good()) { boldPath = p; break; }
        }

        ImFontConfig fontCfg;
        fontCfg.OversampleH       = 2;   // subpixel antialiasing
        fontCfg.OversampleV       = 1;
        fontCfg.PixelSnapH        = true;

        if (regularPath)
        {
            io.Fonts->Clear();
            io.Fonts->AddFontFromFileTTF(regularPath, 15.0f, &fontCfg);
            if (boldPath)
                io.Fonts->AddFontFromFileTTF(boldPath, 15.0f, &fontCfg);
            Logger::info("ImGui: loaded Google Sans Flex from " + std::string(regularPath));
        }
        else
        {
            Logger::info("ImGui: Google Sans Flex not found, using default font");
        }

        initVulkanBackend(swapchainFormat, imageCount);

        // Restore UI preferences from persistent state
        if (pPersistentState)
            visible = pPersistentState->visible;
        setInputBlocked(visible);

        initialized = true;
        Logger::info("ImGui overlay initialized");
    }


    ImGuiOverlay::~ImGuiOverlay()
    {
        if (!initialized) return;

        // Auto-save profile on shutdown (before GPU cleanup)
        if (profileDirty && !activeProfilePath.empty())
            autoSaveProfile();

        pLogicalDevice->vkd.QueueWaitIdle(pLogicalDevice->queue);

        // Clean up Wayland resources before destroying the event queue
        if (isWayland())
            cleanupPointerConstraints();

        std::string iniPath = ConfigSerializer::getBaseConfigDir() + "/imgui.ini";
        ImGui::SaveIniSettingsToDisk(iniPath.c_str());

        if (backendInitialized)
            ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext();

        for (auto fb : framebuffers)
        {
            if (fb != VK_NULL_HANDLE)
                pLogicalDevice->vkd.DestroyFramebuffer(pLogicalDevice->device, fb, nullptr);
        }
        for (auto fence : commandBufferFences)
        {
            if (fence != VK_NULL_HANDLE)
                pLogicalDevice->vkd.DestroyFence(pLogicalDevice->device, fence, nullptr);
        }
        if (commandPool != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyCommandPool(pLogicalDevice->device, commandPool, nullptr);
        if (renderPass != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyRenderPass(pLogicalDevice->device, renderPass, nullptr);
        if (descriptorPool != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyDescriptorPool(pLogicalDevice->device, descriptorPool, nullptr);

        Logger::info("ImGui overlay destroyed");
    }

    void ImGuiOverlay::toggle()
    {
        visible = !visible;
        setInputBlocked(visible);

        // Pointer confinement was removed: the overlay renders inside the game's
        // surface, so confining the pointer doesn't prevent the compositor from
        // interpreting clicks as window-move grabs.

        saveToPersistentState();
    }

    void ImGuiOverlay::saveToPersistentState()
    {
        if (!pPersistentState)
            return;

        // Only save UI preferences - effect state is in the registry, settings in settingsManager
        pPersistentState->visible = visible;
    }

    void ImGuiOverlay::updateState(OverlayState newState)
    {
        state = std::move(newState);

        if (!pEffectRegistry)
            return;

        // Registry is already initialized from config at swapchain creation
        // Just ensure any newly added effects are in the registry
        const auto& selectedEffects = pEffectRegistry->getSelectedEffects();
        for (const auto& effectName : selectedEffects)
        {
            if (!pEffectRegistry->hasEffect(effectName))
                pEffectRegistry->ensureEffect(effectName);
        }
        // No editableParams merging needed - Registry IS the source of truth

        // If Safe Anti-Cheat is active, disable any depth-using effects in the selection
        if (profileSafeAntiCheat)
            disableDepthEffects();
    }

    void ImGuiOverlay::disableDepthEffects()
    {
        if (!pEffectRegistry)
            return;

        ShaderManagerConfig smConfig = ConfigSerializer::loadShaderManagerConfig();
        const auto& selectedEffects = pEffectRegistry->getSelectedEffects();

        for (const auto& effectName : selectedEffects)
        {
            auto it = state.effectPaths.find(effectName);
            if (it == state.effectPaths.end())
                continue;

            bool usesDepth = depthShaders.count(effectName) > 0;
            if (!usesDepth && !shaderTestComplete)
            {
                if (checkShaderUsesDepth(effectName, it->second, smConfig.discoveredShaderPaths))
                {
                    depthShaders.insert(effectName);
                    usesDepth = true;
                }
            }
            if (usesDepth && pEffectRegistry->isEffectEnabled(effectName))
            {
                pEffectRegistry->setEffectEnabled(effectName, false);
                Logger::info("Safe Anti-Cheat: disabled depth effect '" + effectName + "'");
            }
        }
    }

    std::vector<std::unique_ptr<EffectParam>> ImGuiOverlay::getModifiedParams()
    {
        if (!pEffectRegistry)
            return {};
        return pEffectRegistry->getAllParameters();
    }

    std::vector<std::string> ImGuiOverlay::getActiveEffects() const
    {
        std::vector<std::string> activeEffects;
        if (!pEffectRegistry)
            return activeEffects;

        for (const auto& effectName : pEffectRegistry->getSelectedEffects())
        {
            if (pEffectRegistry->isEffectEnabled(effectName))
                activeEffects.push_back(effectName);
        }
        return activeEffects;
    }

    const std::vector<std::string>& ImGuiOverlay::getSelectedEffects() const
    {
        static std::vector<std::string> empty;
        return pEffectRegistry ? pEffectRegistry->getSelectedEffects() : empty;
    }

    void ImGuiOverlay::collectSaveData(
        std::vector<std::string>& effects,
        std::vector<std::string>& disabledEffects,
        std::vector<ConfigParam>& params,
        std::map<std::string, std::string>& effectPaths,
        std::vector<PreprocessorDefinition>& allDefs)
    {
        if (!pEffectRegistry)
            return;

        effects = pEffectRegistry->getSelectedEffects();

        for (const auto& effectName : effects)
        {
            for (auto* p : pEffectRegistry->getParametersForEffect(effectName))
            {
                if (!p->hasChanged())
                    continue;

                auto serialized = p->serialize();
                for (const auto& [suffix, value] : serialized)
                {
                    ConfigParam cp;
                    cp.effectName = p->effectName;
                    cp.paramName = suffix.empty() ? p->name : suffix;
                    cp.value = value;
                    params.push_back(cp);
                }
            }

            if (!pEffectRegistry->isEffectEnabled(effectName))
                disabledEffects.push_back(effectName);

            if (pEffectRegistry->isEffectBuiltIn(effectName))
            {
                std::string effectType = pEffectRegistry->getEffectType(effectName);
                if (!effectType.empty())
                    effectPaths[effectName] = effectType;
            }
            else
            {
                std::string path = pEffectRegistry->getEffectFilePath(effectName);
                if (!path.empty())
                    effectPaths[effectName] = path;

                const auto& defs = pEffectRegistry->getPreprocessorDefs(effectName);
                for (const auto& def : defs)
                    allDefs.push_back(def);
            }
        }
    }

    void ImGuiOverlay::saveCurrentConfig()
    {
        if (!pEffectRegistry)
            return;

        std::vector<std::string> effects, disabledEffects;
        std::vector<ConfigParam> params;
        std::map<std::string, std::string> effectPaths;
        std::vector<PreprocessorDefinition> allDefs;
        collectSaveData(effects, disabledEffects, params, effectPaths, allDefs);

        ConfigSerializer::saveConfig(saveConfigName, effects, disabledEffects, params, effectPaths, allDefs);
        profileDirty = false;
    }

    void ImGuiOverlay::autoSaveProfile()
    {
        if (!pEffectRegistry || activeProfilePath.empty())
            return;

        std::vector<std::string> effects, disabledEffects;
        std::vector<ConfigParam> params;
        std::map<std::string, std::string> effectPaths;
        std::vector<PreprocessorDefinition> allDefs;
        collectSaveData(effects, disabledEffects, params, effectPaths, allDefs);

        ProfileSettings profileSettings;
        profileSettings.safeAntiCheat = profileSafeAntiCheat;

        if (ConfigSerializer::saveToPath(activeProfilePath, effects, disabledEffects, params, effectPaths, allDefs, profileSettings))
        {
            profileDirty = false;
            Logger::debug("Auto-saved profile: " + activeProfilePath);
        }
    }

    void ImGuiOverlay::setSelectedEffects(const std::vector<std::string>& effects,
                                          const std::vector<std::string>& disabledEffects)
    {
        if (!pEffectRegistry)
            return;

        pEffectRegistry->setSelectedEffects(effects);

        // Build set of disabled effects for quick lookup
        std::set<std::string> disabledSet(disabledEffects.begin(), disabledEffects.end());

        // Set enabled states in registry: disabled if in disabledEffects, enabled otherwise
        for (const auto& effectName : effects)
        {
            bool enabled = (disabledSet.find(effectName) == disabledSet.end());
            pEffectRegistry->setEffectEnabled(effectName, enabled);
        }
    }

    void ImGuiOverlay::initVulkanBackend(VkFormat swapchainFormat, uint32_t imageCount)
    {
        // Load Vulkan functions for ImGui using vkShade's dispatch tables
        bool loaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imguiVulkanLoaderDummy, pLogicalDevice);
        if (!loaded)
        {
            Logger::err("Failed to load Vulkan functions for ImGui");
            return;
        }
        Logger::debug("ImGui Vulkan functions loaded");

        // Create descriptor pool for ImGui
        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 }
        };

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 100;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes;

        VkResult vr = pLogicalDevice->vkd.CreateDescriptorPool(pLogicalDevice->device, &poolInfo, nullptr, &descriptorPool);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui descriptor pool: " + std::to_string(vr));
            return;
        }

        // Create render pass for ImGui
        VkAttachmentDescription attachment = {};
        attachment.format = swapchainFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef = {};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        // Dependency for incoming: wait for prior color writes before overlay renders.
        // srcAccessMask MUST include COLOR_ATTACHMENT_WRITE (or TRANSFER_WRITE) so
        // that the implicit PRESENT_SRC→COLOR_ATTACHMENT_OPTIMAL layout transition
        // does not race with the effect pass that just wrote to the swapchain image.
        // The semaphore between effect-submit and overlay-submit guarantees
        // execution ordering, but the render-pass dependency governs the implicit
        // transition barrier and must correctly describe the source access.
        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        // Dependency for outgoing: ensure overlay writes + layout transition complete
        // and are VISIBLE to presentation engine and DMA-BUF screen capture readers.
        // VK_ACCESS_MEMORY_READ_BIT is critical — without it, GPU caches may not be
        // flushed before PipeWire/compositor reads the image via DMA-BUF.
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 2;
        renderPassInfo.pDependencies = dependencies;

        vr = pLogicalDevice->vkd.CreateRenderPass(pLogicalDevice->device, &renderPassInfo, nullptr, &renderPass);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui render pass: " + std::to_string(vr));
            return;
        }

        // Initialize ImGui Vulkan backend
        ImGui_ImplVulkan_InitInfo initInfo = {};
        initInfo.ApiVersion = VK_API_VERSION_1_3;
        initInfo.Instance = pLogicalDevice->instance;
        initInfo.PhysicalDevice = pLogicalDevice->physicalDevice;
        initInfo.Device = pLogicalDevice->device;
        initInfo.QueueFamily = pLogicalDevice->queueFamilyIndex;
        initInfo.Queue = pLogicalDevice->queue;
        initInfo.DescriptorPool = descriptorPool;
        // ImageCount MUST match the actual swapchain image count.
        // Hardcoding 2 while the real swapchain has 3 (triple-buffering)
        // causes ImGui's internal vertex/index ring buffer to wrap too early,
        // overwriting buffers still in-flight on the GPU → ghost trails,
        // stale vertices, and black corruption artefacts.
        initInfo.MinImageCount = imageCount;
        initInfo.ImageCount = imageCount;
        initInfo.PipelineInfoMain.RenderPass = renderPass;

        ImGui_ImplVulkan_Init(&initInfo);

        this->swapchainFormat = swapchainFormat;
        this->imageCount = imageCount;
        bool commandBuffersAllocated = false;

        auto rollbackBackendInit = [&]() {
            for (auto fence : commandBufferFences)
            {
                if (fence != VK_NULL_HANDLE)
                    pLogicalDevice->vkd.DestroyFence(pLogicalDevice->device, fence, nullptr);
            }
            commandBufferFences.clear();

            if (commandBuffersAllocated && !commandBuffers.empty() && commandPool != VK_NULL_HANDLE)
            {
                pLogicalDevice->vkd.FreeCommandBuffers(
                    pLogicalDevice->device, commandPool, static_cast<uint32_t>(commandBuffers.size()), commandBuffers.data());
            }
            commandBuffers.clear();

            for (auto fb : framebuffers)
            {
                if (fb != VK_NULL_HANDLE)
                    pLogicalDevice->vkd.DestroyFramebuffer(pLogicalDevice->device, fb, nullptr);
            }
            framebuffers.clear();
            framebufferImageViews.clear();

            if (commandPool != VK_NULL_HANDLE)
            {
                pLogicalDevice->vkd.DestroyCommandPool(pLogicalDevice->device, commandPool, nullptr);
                commandPool = VK_NULL_HANDLE;
            }
            if (renderPass != VK_NULL_HANDLE)
            {
                pLogicalDevice->vkd.DestroyRenderPass(pLogicalDevice->device, renderPass, nullptr);
                renderPass = VK_NULL_HANDLE;
            }
            if (descriptorPool != VK_NULL_HANDLE)
            {
                pLogicalDevice->vkd.DestroyDescriptorPool(pLogicalDevice->device, descriptorPool, nullptr);
                descriptorPool = VK_NULL_HANDLE;
            }

            ImGui_ImplVulkan_Shutdown();
            backendInitialized = false;
        };

        // Create command pool
        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCreateInfo.queueFamilyIndex = pLogicalDevice->queueFamilyIndex;
        vr = pLogicalDevice->vkd.CreateCommandPool(pLogicalDevice->device, &poolCreateInfo, nullptr, &commandPool);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui command pool: " + std::to_string(vr));
            rollbackBackendInit();
            return;
        }

        // Allocate command buffers
        commandBuffers.resize(imageCount);
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = imageCount;
        vr = pLogicalDevice->vkd.AllocateCommandBuffers(pLogicalDevice->device, &allocInfo, commandBuffers.data());
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to allocate ImGui command buffers: " + std::to_string(vr));
            rollbackBackendInit();
            return;
        }
        commandBuffersAllocated = true;

        // Create fences for command buffer synchronization (signaled initially so first frame doesn't wait)
        commandBufferFences.resize(imageCount);
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < imageCount; i++)
        {
            vr = pLogicalDevice->vkd.CreateFence(pLogicalDevice->device, &fenceInfo, nullptr, &commandBufferFences[i]);
            if (vr != VK_SUCCESS)
            {
                Logger::err("Failed to create ImGui fence " + std::to_string(i) + ": " + std::to_string(vr));
                rollbackBackendInit();
                return;
            }
        }

        backendInitialized = true;
        Logger::debug("ImGui Vulkan backend initialized");
    }

    VkCommandBuffer ImGuiOverlay::recordFrame(uint32_t imageIndex, VkImageView imageView, uint32_t width, uint32_t height)
    {
        if (!backendInitialized || !visible)
            return VK_NULL_HANDLE;

        const bool blockOverlayInput = visible && settingsManager.getOverlayBlockInput();
        setInputBlocked(blockOverlayInput);

        // Store current resolution for VRAM estimates in settings
        currentWidth = width;
        currentHeight = height;

        // Wait for previous use of this command buffer to complete.
        // Adaptive timeout: 4x the measured frame time (generous margin for GPU
        // scheduling jitter), clamped to [2ms, 50ms].  With triple buffering the
        // fence was submitted 2-3 frames ago and is almost always already signaled.
        static float avgFrameTimeMs = 8.0f;  // Seed at ~120 FPS
        VkFence fence = commandBufferFences[imageIndex];
        uint64_t timeoutNs = static_cast<uint64_t>(
            std::clamp(avgFrameTimeMs * 4.0f, 2.0f, 50.0f) * 1'000'000.0f);
        VkResult fenceResult = pLogicalDevice->vkd.WaitForFences(pLogicalDevice->device, 1, &fence, VK_TRUE, timeoutNs);
        if (fenceResult == VK_TIMEOUT)
        {
            Logger::warn("ImGui fence wait timed out for image " + std::to_string(imageIndex));
            return VK_NULL_HANDLE;
        }
        if (fenceResult != VK_SUCCESS)
        {
            Logger::err("ImGui fence wait failed: " + std::to_string(fenceResult));
            return VK_NULL_HANDLE;
        }
        VkResult resetResult = pLogicalDevice->vkd.ResetFences(pLogicalDevice->device, 1, &fence);
        if (resetResult != VK_SUCCESS)
        {
            Logger::err("Failed to reset ImGui fence: " + std::to_string(resetResult));
            return VK_NULL_HANDLE;
        }

        VkCommandBuffer cmd = commandBuffers[imageIndex];

        // Begin command buffer
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult vr = pLogicalDevice->vkd.BeginCommandBuffer(cmd, &beginInfo);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to begin ImGui command buffer: " + std::to_string(vr));
            return VK_NULL_HANDLE;
        }

        // Ensure framebuffer exists for this image index.
        // Recreate if the image view or dimensions changed (swapchain recreation).
        if (framebuffers.size() <= imageIndex)
        {
            framebuffers.resize(imageIndex + 1, VK_NULL_HANDLE);
            framebufferImageViews.resize(imageIndex + 1, VK_NULL_HANDLE);
        }

        bool needRecreate = (framebuffers[imageIndex] == VK_NULL_HANDLE) ||
                            (framebufferImageViews[imageIndex] != imageView) ||
                            (framebufferWidth != width) || (framebufferHeight != height);

        if (needRecreate)
        {
            if (framebuffers[imageIndex] != VK_NULL_HANDLE)
                pLogicalDevice->vkd.DestroyFramebuffer(pLogicalDevice->device, framebuffers[imageIndex], nullptr);

            VkFramebufferCreateInfo fbInfo = {};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = renderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &imageView;
            fbInfo.width = width;
            fbInfo.height = height;
            fbInfo.layers = 1;
            vr = pLogicalDevice->vkd.CreateFramebuffer(pLogicalDevice->device, &fbInfo, nullptr, &framebuffers[imageIndex]);
            if (vr != VK_SUCCESS)
            {
                Logger::err("Failed to create ImGui framebuffer: " + std::to_string(vr));
                pLogicalDevice->vkd.EndCommandBuffer(cmd);
                return VK_NULL_HANDLE;
            }
            framebufferImageViews[imageIndex] = imageView;
            framebufferWidth = width;
            framebufferHeight = height;
        }

        VkFramebuffer framebuffer = framebuffers[imageIndex];

        // Set display size and frame timing BEFORE NewFrame
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)width, (float)height);

        // DeltaTime for ImGui's internal timing (drag thresholds, animations).
        // Uses last known good value as fallback instead of hardcoded 1/60.
        static auto lastFrameTime = std::chrono::steady_clock::now();
        static float lastGoodDt = 0.008f;  // Seed at ~120 FPS
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;
        if (dt > 0.0f && dt < 1.0f)
        {
            io.DeltaTime = dt;
            lastGoodDt = dt;
            // Update adaptive fence timeout with exponential moving average
            avgFrameTimeMs = avgFrameTimeMs * 0.9f + (dt * 1000.0f) * 0.1f;
        }
        else
        {
            io.DeltaTime = lastGoodDt;
        }

        // Mouse input for interactivity
        MouseState mouse = getMouseState();
        io.MousePos = ImVec2((float)mouse.x, (float)mouse.y);

        io.MouseDown[0] = mouse.leftButton;
        io.MouseDown[1] = mouse.rightButton;
        io.MouseDown[2] = mouse.middleButton;
        io.MouseWheel = mouse.scrollDelta;
        // Do NOT draw a software cursor — it trails behind the real cursor and
        // leaves ghost artefacts on frames where the overlay is active.
        // The OS cursor (or compositor cursor on Wayland) is always visible
        // because vkShade sets inputBlocked=false when the overlay is hidden.
        io.MouseDrawCursor = true;

        // Keyboard input for text fields
        // Keys are one-shot events, so we send press and release in same frame
        KeyboardState keyboard = getKeyboardState();
        for (char c : keyboard.typedChars)
            io.AddInputCharacter(c);
        if (keyboard.backspace) { io.AddKeyEvent(ImGuiKey_Backspace, true); io.AddKeyEvent(ImGuiKey_Backspace, false); }
        if (keyboard.del) { io.AddKeyEvent(ImGuiKey_Delete, true); io.AddKeyEvent(ImGuiKey_Delete, false); }
        if (keyboard.enter) { io.AddKeyEvent(ImGuiKey_Enter, true); io.AddKeyEvent(ImGuiKey_Enter, false); }
        if (keyboard.left) { io.AddKeyEvent(ImGuiKey_LeftArrow, true); io.AddKeyEvent(ImGuiKey_LeftArrow, false); }
        if (keyboard.right) { io.AddKeyEvent(ImGuiKey_RightArrow, true); io.AddKeyEvent(ImGuiKey_RightArrow, false); }
        if (keyboard.home) { io.AddKeyEvent(ImGuiKey_Home, true); io.AddKeyEvent(ImGuiKey_Home, false); }
        if (keyboard.end) { io.AddKeyEvent(ImGuiKey_End, true); io.AddKeyEvent(ImGuiKey_End, false); }

        // ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();

        // Clamp overlay window to screen bounds so it can never go offscreen
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 screenMin = viewport->WorkPos;
        ImVec2 screenMax = ImVec2(screenMin.x + viewport->WorkSize.x, screenMin.y + viewport->WorkSize.y);

        // Set default size/position on first appearance
        ImGui::SetNextWindowSizeConstraints(ImVec2(300, 200), ImVec2(screenMax.x - screenMin.x, screenMax.y - screenMin.y));
        if (resetLayoutRequested)
        {
            ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_Always);
            resetLayoutRequested = false;
        }
        else
        {
            ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
        }

        ImGui::Begin("vkShade Overlay", nullptr, ImGuiWindowFlags_NoCollapse);

        // Clamp position after the window is created (prevents dragging offscreen)
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        bool clamped = false;
        if (winPos.x + winSize.x < screenMin.x + 50) { winPos.x = screenMin.x; clamped = true; }
        if (winPos.y < screenMin.y)                   { winPos.y = screenMin.y; clamped = true; }
        if (winPos.x > screenMax.x - 50)              { winPos.x = screenMax.x - 50; clamped = true; }
        if (winPos.y > screenMax.y - 30)              { winPos.y = screenMax.y - 30; clamped = true; }
        if (clamped)
            ImGui::SetWindowPos(winPos);

        // Process shader test (one per frame) regardless of active tab
        processShaderTest();

        // Gather depth-buffer state for the Advanced tab. The overlay shares the
        // process with the layer, so we read LogicalDevice directly under the
        // global lock.
        gatherDepthInfo();
        applyDepthPinRequests();

        if (ImGui::BeginTabBar("OverlayTabs"))
        {
            if (ImGui::BeginTabItem("Effects"))
            {
                if (inSelectionMode)
                    renderAddEffectsView();
                else if (inConfigManageMode)
                    renderConfigManagerView();
                else
                    renderMainView(keyboard);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Shaders"))
            {
                renderShaderManagerView();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Settings"))
            {
                renderSettingsView(keyboard);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Advanced"))
            {
                renderAdvancedView();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Diagnostics"))
            {
                renderDiagnosticsView();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();  // vkShade Overlay

        // Debug window (separate, controlled by setting)
        renderDebugWindow();

        // Global auto-apply check (runs regardless of which tab is active)
        if (settingsManager.getAutoApply() && paramsDirty)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastChangeTime).count();
            const bool leftMouseDown = ImGui::GetIO().MouseDown[0];

            // Defer auto-apply while left mouse is held (dragging/resizing/sliders).
            // Apply on the first frame after release once delay has already elapsed.
            if (elapsed >= settingsManager.getAutoApplyDelay() && !leftMouseDown)
            {
                applyRequested = true;
                paramsDirty = false;
                profileDirty = true;  // Mark for auto-save to profile
            }
        }

        // Auto-save profile when changes are applied
        if (profileDirty && !paramsDirty && !activeProfilePath.empty())
            autoSaveProfile();

        // Focus Effects window on first frame of the session
        static bool firstFrame = true;
        if (firstFrame)
        {
            ImGui::SetWindowFocus("Effects");
            firstFrame = false;
        }

        ImGui::Render();

        // Begin render pass
        VkRenderPassBeginInfo rpBegin = {};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = renderPass;
        rpBegin.framebuffer = framebuffer;
        rpBegin.renderArea.extent.width = width;
        rpBegin.renderArea.extent.height = height;

        pLogicalDevice->vkd.CmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        pLogicalDevice->vkd.CmdEndRenderPass(cmd);

        vr = pLogicalDevice->vkd.EndCommandBuffer(cmd);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to end ImGui command buffer: " + std::to_string(vr));
            return VK_NULL_HANDLE;
        }

        return cmd;
    }

} // namespace vkShade
