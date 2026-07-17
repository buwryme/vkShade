#include "imgui_overlay.hpp"
#include "effects/effect_registry.hpp"
#include "settings_manager.hpp"
#include "reshade_parser.hpp"
#include "config_serializer.hpp"

#include <algorithm>
#include <cstring>
#include <cctype>

#include "imgui/imgui.h"

namespace vkShade
{
    static bool matchesSearch(const std::string& text, const char* search)
    {
        if (!search || !search[0])
            return true;
        std::string lowerText = text;
        std::string lowerSearch = search;
        std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);
        std::transform(lowerSearch.begin(), lowerSearch.end(), lowerSearch.begin(), ::tolower);
        return lowerText.find(lowerSearch) != std::string::npos;
    }

    void ImGuiOverlay::renderAddEffectsView()
    {
        if (!pEffectRegistry)
            return;

        if (profileSafeAntiCheat && !shaderTestComplete && !shaderTestRunning)
            startShaderTest();

        std::vector<std::string> selectedEffects = pEffectRegistry->getSelectedEffects();

        // Handle ESC to clear search
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && addEffectsSearch[0] != '\0')
            addEffectsSearch[0] = '\0';

        // Capture keyboard input for seamless search (only when no widget is active)
        if (!ImGui::IsAnyItemActive())
        {
            ImGuiIO& io = ImGui::GetIO();
            if (io.InputQueueCharacters.Size > 0)
            {
                for (int i = 0; i < io.InputQueueCharacters.Size; i++)
                {
                    ImWchar c = io.InputQueueCharacters[i];
                    if (c >= 32 && c < 127)
                    {
                        size_t len = strlen(addEffectsSearch);
                        if (len < sizeof(addEffectsSearch) - 1)
                        {
                            addEffectsSearch[len] = static_cast<char>(c);
                            addEffectsSearch[len + 1] = '\0';
                        }
                    }
                }
                io.InputQueueCharacters.clear();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && addEffectsSearch[0] != '\0')
            {
                size_t len = strlen(addEffectsSearch);
                if (len > 0)
                    addEffectsSearch[len - 1] = '\0';
            }
        }

        // Add Effects mode
        size_t maxEffectsLimit = static_cast<size_t>(settingsManager.getMaxEffects());
        if (insertPosition >= 0)
            ImGui::Text("Insert Effects at position %d (max %zu)", insertPosition, maxEffectsLimit);
        else
            ImGui::Text("Add Effects (max %zu)", maxEffectsLimit);

        if (shaderTestRunning && profileSafeAntiCheat)
        {
            float progress = shaderTestQueue.empty() ? 1.0f :
                static_cast<float>(shaderTestCurrentIndex) / static_cast<float>(shaderTestQueue.size());
            ImGui::ProgressBar(progress, ImVec2(-1, 0),
                ("Checking shaders " + std::to_string(shaderTestCurrentIndex) + "/" +
                 std::to_string(shaderTestQueue.size())).c_str());
        }
        ImGui::Separator();

        size_t currentCount = selectedEffects.size();
        size_t pendingCount = pendingAddEffects.size();
        size_t totalCount = currentCount + pendingCount;

        std::vector<std::string> builtinEffects = {"cas", "dls", "fxaa", "smaa", "deband", "lut"};

        auto isNameUsed = [&](const std::string& name) {
            if (std::find(selectedEffects.begin(), selectedEffects.end(), name) != selectedEffects.end())
                return true;
            for (const auto& p : pendingAddEffects)
                if (p.first == name)
                    return true;
            return false;
        };

        auto getNextInstanceName = [&](const std::string& effectType) -> std::string {
            if (!isNameUsed(effectType))
                return effectType;
            for (int n = 2; n <= 99; n++)
            {
                std::string candidate = effectType + "." + std::to_string(n);
                if (!isNameUsed(candidate))
                    return candidate;
            }
            return effectType + ".99";
        };

        // Pre-filter all effects to know if there are any matches before calling
        // isDepthEffect (which can trigger synchronous shader compilation).
        auto shouldShow = [&](const std::string& effectType) -> bool {
            return matchesSearch(effectType, addEffectsSearch);
        };

        // Check if an effect uses depth — only relevant when safe anti-cheat is on.
        // NOT called when the search is active but has zero matches (avoids
        // redundant shader compilation on an empty list).
        auto isDepthEffect = [&](const std::string& effectType) -> bool {
            if (!profileSafeAntiCheat)
                return false;
            static const std::set<std::string> safeBuiltins = {"cas", "dls", "fxaa", "smaa", "deband", "lut"};
            if (safeBuiltins.count(effectType))
                return false;
            if (depthShaders.count(effectType))
                return true;
            if (checkedShaders.count(effectType) || shaderTestComplete)
                return false;
            auto it = state.effectPaths.find(effectType);
            if (it == state.effectPaths.end())
                return false;
            checkedShaders.insert(effectType);
            ShaderManagerConfig smConfig = ConfigSerializer::loadShaderManagerConfig();
            if (checkShaderUsesDepth(effectType, it->second, smConfig.discoveredShaderPaths))
            {
                depthShaders.insert(effectType);
                return true;
            }
            return false;
        };

        auto renderAddButton = [&](const std::string& effectType, const std::string& tooltip = "") {
            bool atLimit = totalCount >= maxEffectsLimit;
            bool depthBlocked = profileSafeAntiCheat && isDepthEffect(effectType);

            if (atLimit || depthBlocked)
                ImGui::BeginDisabled();

            if (ImGui::Button(effectType.c_str(), ImVec2(-1, 0)))
            {
                std::string instanceName = getNextInstanceName(effectType);
                pendingAddEffects.push_back({instanceName, effectType});
            }

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (depthBlocked)
                    ImGui::SetTooltip("Requires depth buffer (blocked by Safe Anti-Cheat)");
                else if (!tooltip.empty())
                    ImGui::SetTooltip("%s", tooltip.c_str());
            }

            if (atLimit || depthBlocked)
                ImGui::EndDisabled();
        };

        // Two column layout
        float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        float contentHeight = -footerHeight;
        float columnWidth = ImGui::GetContentRegionAvail().x * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f;
        if (columnWidth < 100.0f)
            columnWidth = 100.0f;

        // Left column: Available effects
        ImGui::BeginChild("EffectList", ImVec2(columnWidth, contentHeight), true);

        bool hasSearch = addEffectsSearch[0] != '\0';

        // Always show the search bar (not just when searching)
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.3f, 1.0f));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##search", addEffectsSearch, sizeof(addEffectsSearch), ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleColor();
        if (!hasSearch)
            ImGui::TextDisabled("Type to filter...");
        else
            ImGui::TextDisabled("ESC to clear");
        ImGui::Separator();

        // Sort effects for each category
        std::vector<std::string> sortedCurrentConfig = state.currentConfigEffects;
        std::vector<std::string> sortedDefaultConfig = state.defaultConfigEffects;
        std::sort(sortedCurrentConfig.begin(), sortedCurrentConfig.end());
        std::sort(sortedDefaultConfig.begin(), sortedDefaultConfig.end());

        // Count total visible effects for the "no results" message
        size_t totalVisible = 0;

        // Built-in effects (filtered)
        for (const auto& effectType : builtinEffects)
        {
            if (!shouldShow(effectType))
                continue;
            if (!hasSearch)
                ImGui::Text("Built-in:");
            for (const auto& et : builtinEffects)
            {
                if (shouldShow(et))
                {
                    renderAddButton(et);
                    totalVisible++;
                }
            }
            break;
        }

        // ReShade effects from current config (filtered)
        for (const auto& effectType : sortedCurrentConfig)
        {
            if (!shouldShow(effectType))
                continue;
            ImGui::Separator();
            if (!hasSearch)
                ImGui::Text("ReShade (%s):", state.configName.c_str());
            for (const auto& et : sortedCurrentConfig)
            {
                if (!shouldShow(et))
                    continue;
                auto it = state.effectPaths.find(et);
                std::string path = (it != state.effectPaths.end()) ? it->second : "";
                renderAddButton(et, path);
                totalVisible++;
            }
            break;
        }

        // ReShade effects from default config (filtered)
        for (const auto& effectType : sortedDefaultConfig)
        {
            if (!shouldShow(effectType))
                continue;
            ImGui::Separator();
            if (!hasSearch)
                ImGui::Text("ReShade (all):");
            for (const auto& et : sortedDefaultConfig)
            {
                if (!shouldShow(et))
                    continue;
                // Skip if already shown in current config
                if (std::find(sortedCurrentConfig.begin(), sortedCurrentConfig.end(), et) != sortedCurrentConfig.end())
                    continue;
                auto it = state.effectPaths.find(et);
                std::string path = (it != state.effectPaths.end()) ? it->second : "";
                renderAddButton(et, path);
                totalVisible++;
            }
            break;
        }

        if (totalVisible == 0)
            ImGui::TextDisabled("No effects match '%s'", addEffectsSearch);

        ImGui::EndChild();

        ImGui::SameLine();

        // Right column: Pending effects
        ImGui::BeginChild("PendingList", ImVec2(columnWidth, contentHeight), true);
        ImGui::Text("Will add (%zu):", pendingCount);
        ImGui::Separator();

        for (size_t i = 0; i < pendingAddEffects.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("x"))
            {
                pendingAddEffects.erase(pendingAddEffects.begin() + i);
                ImGui::PopID();
                continue;
            }
            ImGui::SameLine();
            const auto& [instanceName, effectType] = pendingAddEffects[i];
            if (instanceName != effectType)
                ImGui::Text("%s (%s)", instanceName.c_str(), effectType.c_str());
            else
                ImGui::Text("%s", instanceName.c_str());
            ImGui::PopID();
        }

        if (pendingAddEffects.empty())
            ImGui::TextDisabled("Click effects to add...");

        ImGui::EndChild();

        ImGui::Separator();

        if (ImGui::Button("Done"))
        {
            int pos = (insertPosition >= 0 && insertPosition <= static_cast<int>(selectedEffects.size()))
                      ? insertPosition : static_cast<int>(selectedEffects.size());
            for (const auto& [instanceName, effectType] : pendingAddEffects)
            {
                selectedEffects.insert(selectedEffects.begin() + pos, instanceName);
                pos++;
                pEffectRegistry->ensureEffect(instanceName, effectType);
                pEffectRegistry->setEffectEnabled(instanceName, true);
            }
            if (!pendingAddEffects.empty())
            {
                pEffectRegistry->setSelectedEffects(selectedEffects);
                applyRequested = true;
                profileDirty = true;
            }
            pendingAddEffects.clear();
            insertPosition = -1;
            inSelectionMode = false;
            addEffectsSearch[0] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            pendingAddEffects.clear();
            insertPosition = -1;
            inSelectionMode = false;
            addEffectsSearch[0] = '\0';
        }
    }

} // namespace vkShade
