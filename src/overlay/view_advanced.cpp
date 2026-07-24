#include "imgui_overlay.hpp"
#include "settings_manager.hpp"
#include "logger.hpp"

#include <algorithm>
#include <sstream>
#include <atomic>

#include "imgui/imgui.h"

namespace VKIntox
{
    // Dirty flag for deferred saving — avoids blocking the UI thread with
    // synchronous disk I/O on every single checkbox/radio/combo change.
    // The actual save happens once per frame in renderAdvancedView().
    static std::atomic<bool> g_settingsDirty{false};
    
    // Mark settings as needing save (call from UI callbacks)
    static inline void markSettingsDirty()
    {
        g_settingsDirty.store(true, std::memory_order_relaxed);
    }
    
    // Perform deferred save if dirty (call once per frame)
    static inline void flushSettingsSaveIfNeeded()
    {
        if (g_settingsDirty.exchange(false, std::memory_order_acq_rel))
        {
            settingsManager.save();
        }
    }
    // Convert a VkFormat to a short human-readable string for the UI.
    static const char* depthFormatName(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_D16_UNORM:           return "D16_UNORM";
        case VK_FORMAT_X8_D24_UNORM_PACK32: return "X8_D24_PACK32";
        case VK_FORMAT_D32_SFLOAT:          return "D32_SFLOAT";
        case VK_FORMAT_D16_UNORM_S8_UINT:   return "D16_UNORM_S8_UINT";
        case VK_FORMAT_D24_UNORM_S8_UINT:   return "D24_UNORM_S8_UINT";
        case VK_FORMAT_D32_SFLOAT_S8_UINT:  return "D32_SFLOAT_S8_UINT";
        case VK_FORMAT_R32_SFLOAT:          return "R32_SFLOAT (resolved)";
        case VK_FORMAT_UNDEFINED:           return "none";
        default:                            return "other";
        }
    }

    static const char* layoutName(VkImageLayout layout)
    {
        switch (layout)
        {
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DS_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:         return "D_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:  return "DS_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:          return "D_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_GENERAL:                          return "GENERAL";
        case VK_IMAGE_LAYOUT_UNDEFINED:                        return "undefined";
        default:                                               return "other";
        }
    }

    static const char* sampleName(VkSampleCountFlagBits samples)
    {
        switch (samples)
        {
        case VK_SAMPLE_COUNT_1_BIT:  return "1x";
        case VK_SAMPLE_COUNT_2_BIT:  return "2x";
        case VK_SAMPLE_COUNT_4_BIT:  return "4x";
        case VK_SAMPLE_COUNT_8_BIT:  return "8x";
        case VK_SAMPLE_COUNT_16_BIT: return "16x";
        case VK_SAMPLE_COUNT_32_BIT: return "32x";
        case VK_SAMPLE_COUNT_64_BIT: return "64x";
        default:                     return "?";
        }
    }

    void ImGuiOverlay::gatherDepthInfo()
    {
        if (!pLogicalDevice)
            return;

        std::lock_guard<std::mutex> l(globalLock);

        depthInfo.supportedResolveModes = pLogicalDevice->supportedDepthResolveModes;
        depthInfo.depthCaptureEnabled   = settingsManager.getDepthCapture();

        const int modePref = settingsManager.getDepthResolveMode();
        const bool avgSupported = (pLogicalDevice->supportedDepthResolveModes & VK_RESOLVE_MODE_AVERAGE_BIT) != 0;
        if (modePref == 2 && avgSupported)
            depthInfo.depthResolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        else
            depthInfo.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

        // Snapshot pin state from the device.  If the pinned view was destroyed
        // by the render thread since last frame, clear the stale pin here so the
        // UI immediately falls back to auto instead of showing a ghost pin.
        depthInfo.pinnedView = pLogicalDevice->pinnedDepthImageView;
        depthInfo.depthIsPinned = (depthInfo.pinnedView != VK_NULL_HANDLE);

        if (depthInfo.depthIsPinned)
        {
            auto it = pLogicalDevice->depthViewStates.find(depthInfo.pinnedView);
            if (it != pLogicalDevice->depthViewStates.end())
            {
                // Pinned view is still valid — show it as the active buffer.
                const DepthState& pinned = it->second;
                depthInfo.active.imageView       = pinned.imageView;
                depthInfo.active.format          = pinned.format;
                depthInfo.active.extent          = pinned.extent;
                depthInfo.active.samples         = pinned.samples;
                depthInfo.active.observedLayout  = pinned.observedLayout;
                depthInfo.active.transient       = pinned.transient;
                depthInfo.active.drawCount = pLogicalDevice->bestDepthCandidate.valid
                                              && pLogicalDevice->bestDepthCandidate.depthState.imageView == pinned.imageView
                                              ? pLogicalDevice->bestDepthCandidate.drawCount : 0;
                depthInfo.active.hasPresentableSnapshotTarget =
                    pLogicalDevice->bestDepthCandidate.valid
                    && pLogicalDevice->bestDepthCandidate.depthState.imageView == pinned.imageView
                    ? pLogicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget : false;
            }
            else
            {
                // Pinned view was destroyed since last frame — clear the stale pin
                // so we fall back to auto.  getDepthState() in vkintox.cpp does the
                // same check, but we need it here too so the UI is consistent.
                Logger::debug("gatherDepthInfo: pinned view no longer tracked, clearing stale pin");
                pLogicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
                depthInfo.depthIsPinned = false;
                depthInfo.pinnedView    = VK_NULL_HANDLE;
                // Don't set active here — fall through to the auto path below.
            }
        }

        // Auto path (also handles the case where the pin was just cleared above).
        if (!depthInfo.depthIsPinned)
        {
            const DepthState& active = pLogicalDevice->activeDepthState;
            depthInfo.active.imageView       = active.imageView;
            depthInfo.active.format          = active.format;
            depthInfo.active.extent          = active.extent;
            depthInfo.active.samples         = active.samples;
            depthInfo.active.observedLayout  = active.observedLayout;
            depthInfo.active.transient       = active.transient;
            depthInfo.active.drawCount       = pLogicalDevice->bestDepthCandidate.valid
                                                 ? pLogicalDevice->bestDepthCandidate.drawCount : 0;
            depthInfo.active.hasPresentableSnapshotTarget =
                pLogicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget;
        }
        depthInfo.depthResolveIsMsaa = depthInfo.active.samples != VK_SAMPLE_COUNT_1_BIT;

        // Populate candidate list from all tracked depth views.
        depthInfo.candidates.clear();
        for (const auto& [view, ds] : pLogicalDevice->depthViewStates)
        {
            DepthCandidateInfo c;
            c.imageView      = view;
            c.format         = ds.format;
            c.extent         = ds.extent;
            c.samples        = ds.samples;
            c.observedLayout = ds.observedLayout;
            c.transient      = ds.transient;
            if (pLogicalDevice->bestDepthCandidate.valid &&
                pLogicalDevice->bestDepthCandidate.depthState.imageView == view)
            {
                c.drawCount = pLogicalDevice->bestDepthCandidate.drawCount;
                c.hasPresentableSnapshotTarget =
                    pLogicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget;
            }
            depthInfo.candidates.push_back(c);
        }
        // Sort by drawCount descending so the best candidate appears first.
        std::sort(depthInfo.candidates.begin(), depthInfo.candidates.end(),
                  [](const DepthCandidateInfo& a, const DepthCandidateInfo& b){
                      return a.drawCount > b.drawCount;
                  });
    }

    void ImGuiOverlay::applyDepthPinRequests()
    {
        if (!pLogicalDevice)
            return;

        if (depthPinPendingView != VK_NULL_HANDLE)
        {
            std::lock_guard<std::mutex> l(globalLock);
            auto it = pLogicalDevice->depthViewStates.find(depthPinPendingView);
            if (it != pLogicalDevice->depthViewStates.end())
            {
                pLogicalDevice->pinnedDepthImageView = depthPinPendingView;
                Logger::info("depth manual pin set to view 0x" +
                    std::to_string(reinterpret_cast<uintptr_t>(depthPinPendingView)));
                depthPinChanged = true;
                // Force deferred realloc so descriptor sets + MSAA framebuffers
                // get rebuilt for the new depth source view.
                pLogicalDevice->depthReallocPending = true;
            }
            else
            {
                Logger::warn("depth pin requested but view not found (may have been destroyed)");
            }
            depthPinPendingView = VK_NULL_HANDLE;
        }

        if (depthPinPendingClear)
        {
            std::lock_guard<std::mutex> l(globalLock);
            pLogicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            depthPinPendingClear = false;
            depthPinChanged = true;
            // Same: force realloc so we stop using the just-unpinned view.
            pLogicalDevice->depthReallocPending = true;
            Logger::info("depth manual pin cleared (back to auto-promotion)");
        }
    }

    void ImGuiOverlay::renderAdvancedView()
    {
        ImGui::BeginChild("AdvancedContent", ImVec2(0, 0), false);

        // --- Status header ---
        ImGui::TextDisabled("Depth buffer capture and selection.");
        ImGui::Spacing();

        if (!depthInfo.depthCaptureEnabled)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "Depth capture is OFF. Enable it in Settings (requires restart).");
            ImGui::Spacing();
        }

        // --- Active depth buffer info ---
        ImGui::Separator();
        if (depthInfo.active.imageView == VK_NULL_HANDLE)
        {
            ImGui::TextDisabled("No depth buffer detected yet.");
            ImGui::TextDisabled("Render a frame with depth to populate this.");
        }
        else
        {
            ImGui::Text("Active: %s  %ux%u  %s",
                depthFormatName(depthInfo.active.format),
                depthInfo.active.extent.width, depthInfo.active.extent.height,
                sampleName(depthInfo.active.samples));
            if (depthInfo.depthResolveIsMsaa)
            {
                const char* mode = (depthInfo.depthResolveMode == VK_RESOLVE_MODE_AVERAGE_BIT) ? "average" : "sample-zero";
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "[MSAA: %s]", mode);
            }
            if (depthInfo.depthIsPinned)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "[PINNED]");
            }
        }

        // --- Selection mode ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool isPinned = depthInfo.depthIsPinned;

        if (ImGui::RadioButton("Auto (best candidate)", !isPinned))
        {
            if (isPinned)
                depthPinPendingClear = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Automatically pick the depth buffer with the most draws\nor a swapchain-linked snapshot target.");

        ImGui::SameLine(0, 16);
        if (ImGui::RadioButton("Manual pin", isPinned))
        {
            // No-op: user must click Pin on a specific row below to set a pin.
            // If already pinned, they pick a different row to change the pin.
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pin a specific depth buffer from the list below.\nOverrides auto-promotion. If the pinned buffer is destroyed,\nfalls back to auto automatically.");

        // Pinned indicator + clear button
        if (isPinned)
        {
            ImGui::Spacing();
            ImGui::Indent(8);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.8f, 0.4f, 1.0f));
            ImGui::Text("Pinned: %s  %ux%u  %s",
                depthFormatName(depthInfo.active.format),
                depthInfo.active.extent.width, depthInfo.active.extent.height,
                sampleName(depthInfo.active.samples));
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 12);
            if (ImGui::Button("Clear Pin"))
                depthPinPendingClear = true;
            ImGui::Unindent(8);
        }

        // --- Candidate table ---
        ImGui::Spacing();
        ImGui::Spacing();

        if (!depthInfo.candidates.empty())
        {
            ImGui::Text("Tracked Depth Buffers (%zu)", depthInfo.candidates.size());

            if (ImGui::BeginTable("##depth_tbl", 5,
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min(depthInfo.candidates.size() + 1, static_cast<size_t>(8))) + 4)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 28);
                ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthStretch, 0.2f);
                ImGui::TableSetupColumn("Info",   ImGuiTableColumnFlags_WidthStretch, 0.3f);
                ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 52);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < depthInfo.candidates.size(); ++i)
                {
                    const DepthCandidateInfo& c = depthInfo.candidates[i];
                    // Compare by VkImageView handle — multiple distinct views can
                    // share the same format:WxHxSamples but point to different images.
                    const bool isActive  = (c.imageView == depthInfo.active.imageView);
                    const bool thisPinned = isPinned && (c.imageView == depthInfo.pinnedView);

                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(i));

                    // Highlight color for active / pinned rows
                    if (isActive)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
                    else if (thisPinned)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.8f, 0.4f, 1.0f));

                    // Column 0: index
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%zu", i);

                    // Column 1: format
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", depthFormatName(c.format));

                    // Column 2: size
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%ux%u", c.extent.width, c.extent.height);

                    // Column 3: info badges
                    ImGui::TableSetColumnIndex(3);
                    {
                        std::string info;
                        info += sampleName(c.samples);
                        if (c.transient)
                            info += "  transient";
                        if (c.hasPresentableSnapshotTarget)
                            info += "  [swapchain]";
                        if (isActive)
                            info += "  ACTIVE";
                        else if (thisPinned)
                            info += "  PINNED";
                        ImGui::Text("%s", info.c_str());
                    }

                    // Column 4: Pin/Unpin button
                    ImGui::TableSetColumnIndex(4);
                    if (thisPinned)
                    {
                        if (ImGui::Button("Unpin", ImVec2(-FLT_MIN, 0)))
                            depthPinPendingClear = true;
                    }
                    else
                    {
                        if (ImGui::Button("Pin", ImVec2(-FLT_MIN, 0)))
                            depthPinPendingView = c.imageView;
                    }

                    // Tooltip on hover over the row (the button is the last widget)
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("View:      0x%llx", reinterpret_cast<unsigned long long>(c.imageView));
                        ImGui::Text("Layout:    %s", layoutName(c.observedLayout));
                        ImGui::Text("Draws:     %u", c.drawCount);
                        ImGui::Text("Transient: %s", c.transient ? "yes" : "no");
                        ImGui::EndTooltip();
                    }

                    if (isActive || thisPinned)
                        ImGui::PopStyleColor();

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
        }
        else
        {
            ImGui::TextDisabled("No depth views tracked yet. Render a frame with depth.");
        }

        // --- MSAA resolve mode ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool avgSupported = (depthInfo.supportedResolveModes & VK_RESOLVE_MODE_AVERAGE_BIT) != 0;
        int modePref = settingsManager.getDepthResolveMode();

        ImGui::Text("MSAA Resolve Mode");
        ImGui::Spacing();

        if (ImGui::RadioButton("Auto##resolveauto", modePref == 0))
        { settingsManager.setDepthResolveMode(0); markSettingsDirty(); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Prefers average when the device supports it.");

        ImGui::SameLine(0, 16);
        if (ImGui::RadioButton("Sample Zero##resolvezero", modePref == 1))
        { settingsManager.setDepthResolveMode(1); markSettingsDirty(); }

        ImGui::SameLine(0, 16);
        ImGui::BeginDisabled(!avgSupported);
        if (ImGui::RadioButton("Average##resolveavg", modePref == 2))
        { settingsManager.setDepthResolveMode(2); markSettingsDirty(); }
        ImGui::EndDisabled();

        if (!avgSupported)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Average mode not supported by this device.");
        }

        // --- Alternative depth buffer handling ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Depth Source Mode (Deferred Rendering)");
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Select the depth buffer encoding used by this application:");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Standard:");
            ImGui::BulletText("Luminance/Red: Standard Vulkan depth (R channel)");
            ImGui::BulletText("Alpha: Depth encoded in alpha channel");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Deferred Renderers:");
            ImGui::BulletText("Packed RGB: Depth distributed across RGB");
            ImGui::BulletText("Logarithmic: Log-encoded depth values");
            ImGui::BulletText("View-space Z: Raw view-space Z value");
            ImGui::BulletText("NDC: Normalized Device Coordinates");
            ImGui::BulletText("Reversed-Z: Inverted for precision");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "Requires restart to take effect.");
            ImGui::EndTooltip();
        }
        ImGui::Spacing();

        // Depth mode names for UI
        const char* depthModeNames[] = {
            "Luminance/Red (standard)",
            "Alpha (alpha-encoded)",
            "Packed RGB",
            "Logarithmic",
            "View-space Z",
            "NDC (Normalized Device Coords)",
            "Reversed-Z"
        };
        
        int dsc = settingsManager.getDepthSourceChannel();
        
        // Use combo box for cleaner UI with many options
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::Combo("##depthSourceChannelCombo", &dsc, depthModeNames, IM_ARRAYSIZE(depthModeNames)))
        {
            settingsManager.setDepthSourceChannel(dsc);
            markSettingsDirty();
        }
        if (ImGui::IsItemHovered())
        {
            const char* tooltips[] = {
                "Read depth from R channel (luminance). Standard Vulkan depth format.",
                "Read depth from Alpha channel. Used by some custom renderers.",
                "Decode depth from packed RGB channels (e.g., RGB24/RGB32 encoding).",
                "Linearize logarithmic depth encoding (exp() linearization).",
                "Convert raw view-space Z to [0,1] range.",
                "Handle NDC depth values (post-projection coordinates).",
                "Reversed-Z encoding for improved depth precision."
            };
            ImGui::SetTooltip("%s", tooltips[dsc]);
        }

        ImGui::Spacing();
        ImGui::Separator();

        bool depthInvert = settingsManager.getDepthInvert();
        if (ImGui::Checkbox("Invert Depth Values##depthInvertToggle", &depthInvert))
        {
            settingsManager.setDepthInvert(depthInvert);
            markSettingsDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Invert depth values: depth = 1.0 - depth");
            ImGui::Text("Flips the near/far plane interpretation.");
            ImGui::Text("Useful when:");
            ImGui::BulletText("Depth buffer uses reversed-Z encoding");
            ImGui::BulletText("Effects expect inverted depth range");
            ImGui::BulletText("Visual debugging of depth precision");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Can be toggled at runtime!");
            ImGui::EndTooltip();
        }

        // Visual indicator for current mode
        ImGui::Indent();
        {
            static const char* modeShortNames[] = {"R", "A", "RGB", "Log", "ViewZ", "NDC", "RevZ"};
            ImGui::TextDisabled("Current: %s%s%s", 
                depthModeNames[dsc],
                depthInvert ? ", INVERTED" : "",
                depthInvert ? " [INV]" : "");
        }
        ImGui::Unindent();

        // --- Transient workaround ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Depth Capture Method ---
        ImGui::Text("Depth Capture Method");
        ImGui::Spacing();

        int dcm = settingsManager.getDepthCaptureMethod();
        if (ImGui::RadioButton("Off (legacy resolve only)##dcm0", dcm == 0))
        { settingsManager.setDepthCaptureMethod(0); markSettingsDirty(); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use the legacy per-swapchain resolve path only.\nNo persistent depth storage.");

        ImGui::SameLine(0, 16);
        if (ImGui::RadioButton("Option A: RenderPass End##dcm1", dcm == 1))
        { settingsManager.setDepthCaptureMethod(1); markSettingsDirty(); }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Blit depth to a persistent storage image at each CmdEndRenderPass.");
            ImGui::Text("Low overhead (~1-2ms/frame). Recommended for most games.");
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Auto-switches to Option B after 30s if no main-res captures.");
            ImGui::EndTooltip();
        }

        ImGui::SameLine(0, 16);
        if (ImGui::RadioButton("Option B: QueueSubmit##dcm2", dcm == 2))
        { settingsManager.setDepthCaptureMethod(2); markSettingsDirty(); }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Intercept QueueSubmit to inject depth blit into the command stream.");
            ImGui::Text("More robust — catches edge cases where CmdEndRenderPass is bypassed.");
            ImGui::Text("Slightly higher overhead due to command buffer analysis per frame.");
            ImGui::EndTooltip();
        }

        ImGui::Spacing();

        bool transientWorkaround = settingsManager.getDepthTransientWorkaround();
        if (ImGui::Checkbox("Transient attachment workaround", &transientWorkaround))
        {
            settingsManager.setDepthTransientWorkaround(transientWorkaround);
            markSettingsDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Forces STORE on depth attachments and tracks transient (lazy-allocated) depth images.");
            ImGui::Text("Enable for games that render MSAA depth with transient attachments (e.g. Roblox via Sober).");
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "StoreOp forcing is always on; this adds extra tracking.");
            ImGui::EndTooltip();
        }

        // --- Force re-detect ---
        ImGui::Spacing();
        if (ImGui::Button("Force Re-detect"))
        {
            std::lock_guard<std::mutex> l(globalLock);
            pLogicalDevice->activeDepthState    = DepthState{};
            pLogicalDevice->bestDepthCandidate  = LogicalDevice::DepthCandidateTrackingState{};
            pLogicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            depthPinChanged = true;
            Logger::info("depth re-detect requested from Advanced UI");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clear active depth buffer, best candidate, and any pin.\nThe layer will re-evaluate all render passes next frame.");

        ImGui::Spacing();
        ImGui::TextDisabled("HW resolve modes: 0x%x", depthInfo.supportedResolveModes);

        // --- Deferred save ---
        // Perform the actual file I/O exactly once per frame, regardless of how
        // many UI widgets changed.  This eliminates the per-widget disk I/O that
        // was causing the Advanced tab to freeze/hang on interaction.
        flushSettingsSaveIfNeeded();

        ImGui::EndChild();
    }

} // namespace VKIntox