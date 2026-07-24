#include "../field_editor.hpp"
#include "../../../imgui/imgui.h"
#include "../../../imgui/imgui_internal.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

namespace VKIntox
{
    // Robust float field editor with custom value input via right-click
    // Uses deferred modal opening to avoid ImGui nested-popup issues
    class FloatFieldEditor : public FieldEditor
    {
    public:
        bool render(EffectParam& param) override
        {
            auto& p = static_cast<FloatParam&>(param);
            bool changed = false;

            // Generate stable unique IDs using parameter address (avoids label collisions)
            const void* paramAddr = static_cast<const void*>(&param);
            char contextId[64];
            char modalId[64];
            char inputId[64];
            snprintf(contextId, sizeof(contextId), "##flctx_%p", paramAddr);
            snprintf(modalId, sizeof(modalId), "##flmodal_%p", paramAddr);
            snprintf(inputId, sizeof(inputId), "##flinput_%p", paramAddr);

            // === Render the slider ===
            if (ImGui::SliderFloat(p.label.c_str(), &p.value, p.minValue, p.maxValue))
            {
                if (p.step > 0.0f)
                    p.value = std::round(p.value / p.step) * p.step;
                changed = true;
            }

            // === Right-click context menu ===
            if (ImGui::BeginPopupContextItem(contextId))
            {
                if (ImGui::MenuItem("Enter Custom Value..."))
                {
                    // Mark modal as pending for THIS parameter, initialize buffer
                    m_activeModalParam = paramAddr;
                    snprintf(m_customValueBuf, sizeof(m_customValueBuf), "%.6g", p.value);
                    // Don't call OpenPopup here - defer to after context menu closes
                    m_modalOpenRequested = true;
                }
                if (ImGui::MenuItem("Reset to default"))
                {
                    resetToDefault(param);
                    changed = true;
                }
                ImGui::EndPopup();
            }

            // === Deferred modal opening (must happen outside any popup context) ===
            if (m_modalOpenRequested && m_activeModalParam == paramAddr)
            {
                m_modalOpenRequested = false;
                ImGui::OpenPopup(modalId);
            }

            // === Render modal popup for THIS parameter only ===
            if (m_activeModalParam == paramAddr && 
                ImGui::BeginPopupModal(modalId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Custom value: %s", p.label.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("Current range: [%.4g, %.4g]", p.minValue, p.maxValue);
                ImGui::TextDisabled("(values outside range are allowed)");
                ImGui::Spacing();
                
                ImGui::SetNextItemWidth(220.0f);
                bool submit = ImGui::InputText(inputId, m_customValueBuf, 
                    sizeof(m_customValueBuf), ImGuiInputTextFlags_EnterReturnsTrue);

                ImGui::Spacing();
                bool ok = ImGui::Button("OK", ImVec2(100, 0));
                ImGui::SameLine();
                bool cancel = ImGui::Button("Cancel", ImVec2(100, 0));

                // Handle submission
                if (submit || ok)
                {
                    float newVal;
                    if (sscanf(m_customValueBuf, "%f", &newVal) == 1)
                    {
                        p.value = newVal;
                        changed = true;
                    }
                    ImGui::CloseCurrentPopup();
                    m_activeModalParam = nullptr;  // Clear active state
                }

                // Handle cancellation
                if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    ImGui::CloseCurrentPopup();
                    m_activeModalParam = nullptr;  // Clear active state
                }
                
                ImGui::EndPopup();
            }

            return changed;
        }

        void resetToDefault(EffectParam& param) override
        {
            param.resetToDefault();
        }

    private:
        static constexpr size_t BUF_SIZE = 128;
        char m_customValueBuf[BUF_SIZE] = "";
        const void* m_activeModalParam = nullptr;  // Which param has modal open
        bool m_modalOpenRequested = false;         // Deferred open flag
    };

    REGISTER_FIELD_EDITOR(ParamType::Float, FloatFieldEditor)

} // namespace VKIntox
