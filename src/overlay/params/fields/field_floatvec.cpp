#include "../field_editor.hpp"
#include "../../../imgui/imgui.h"
#include "../../../imgui/imgui_internal.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

namespace VKIntox
{
    // Robust float vector (vec2/3/4) field editor with custom value input via right-click
    class FloatVecFieldEditor : public FieldEditor
    {
    public:
        bool render(EffectParam& param) override
        {
            auto& p = static_cast<FloatVecParam&>(param);
            bool changed = false;

            // Generate stable unique IDs using parameter address
            const void* paramAddr = static_cast<const void*>(&param);
            char contextId[64];
            char modalId[64];
            char inputId[64];
            snprintf(contextId, sizeof(contextId), "##fvcctx_%p", paramAddr);
            snprintf(modalId, sizeof(modalId), "##fvcmodal_%p", paramAddr);
            snprintf(inputId, sizeof(inputId), "##fvcinput_%p", paramAddr);

            // === Render the vector slider ===
            switch (p.componentCount)
            {
                case 2:
                    changed = ImGui::SliderFloat2(p.label.c_str(), p.value, 
                        p.minValue[0], p.maxValue[0]);
                    break;
                case 3:
                    changed = ImGui::SliderFloat3(p.label.c_str(), p.value,
                        p.minValue[0], p.maxValue[0]);
                    break;
                case 4:
                    changed = ImGui::SliderFloat4(p.label.c_str(), p.value,
                        p.minValue[0], p.maxValue[0]);
                    break;
                default:
                    break;
            }

            if (changed && p.step > 0.0f)
            {
                for (uint32_t i = 0; i < p.componentCount; i++)
                    p.value[i] = std::round(p.value[i] / p.step) * p.step;
            }

            // === Right-click context menu ===
            if (ImGui::BeginPopupContextItem(contextId))
            {
                if (ImGui::MenuItem("Enter Custom Value..."))
                {
                    m_activeModalParam = paramAddr;
                    // Format current values into buffer based on component count
                    formatValues(p);
                    m_modalOpenRequested = true;
                }
                if (ImGui::MenuItem("Reset to default"))
                {
                    resetToDefault(param);
                    changed = true;
                }
                ImGui::EndPopup();
            }

            // === Deferred modal opening ===
            if (m_modalOpenRequested && m_activeModalParam == paramAddr)
            {
                m_modalOpenRequested = false;
                ImGui::OpenPopup(modalId);
            }

            // === Render modal popup ===
            if (m_activeModalParam == paramAddr &&
                ImGui::BeginPopupModal(modalId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Custom values: %s", p.label.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("Components: %u (comma-separated)", 
                    static_cast<unsigned int>(p.componentCount));
                ImGui::TextDisabled("(values outside range are allowed)");
                ImGui::Spacing();

                ImGui::SetNextItemWidth(280.0f);
                bool submit = ImGui::InputText(inputId, m_customValueBuf,
                    sizeof(m_customValueBuf), ImGuiInputTextFlags_EnterReturnsTrue);

                ImGui::Spacing();
                bool ok = ImGui::Button("OK", ImVec2(100, 0));
                ImGui::SameLine();
                bool cancel = ImGui::Button("Cancel", ImVec2(100, 0));

                if (submit || ok)
                {
                    changed |= parseAndApply(p);
                    ImGui::CloseCurrentPopup();
                    m_activeModalParam = nullptr;
                }

                if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    ImGui::CloseCurrentPopup();
                    m_activeModalParam = nullptr;
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
        // Format current vector values into the buffer as comma-separated
        void formatValues(const FloatVecParam& p)
        {
            switch (p.componentCount)
            {
                case 2:
                    snprintf(m_customValueBuf, sizeof(m_customValueBuf),
                        "%.6g, %.6g", p.value[0], p.value[1]);
                    break;
                case 3:
                    snprintf(m_customValueBuf, sizeof(m_customValueBuf),
                        "%.6g, %.6g, %.6g", p.value[0], p.value[1], p.value[2]);
                    break;
                case 4:
                    snprintf(m_customValueBuf, sizeof(m_customValueBuf),
                        "%.6g, %.6g, %.6g, %.6g", 
                        p.value[0], p.value[1], p.value[2], p.value[3]);
                    break;
                default:
                    snprintf(m_customValueBuf, sizeof(m_customValueBuf), "%.6g", p.value[0]);
                    break;
            }
        }

        // Parse comma-separated values and apply to parameter
        bool parseAndApply(FloatVecParam& p)
        {
            float vals[4] = { p.value[0], p.value[1], p.value[2], p.value[3] };
            int count = sscanf(m_customValueBuf, "%f, %f, %f, %f",
                &vals[0], &vals[1], &vals[2], &vals[3]);

            if (count >= static_cast<int>(p.componentCount))
            {
                for (uint32_t i = 0; i < p.componentCount; i++)
                    p.value[i] = vals[i];
                return true;
            }
            return false;
        }

        static constexpr size_t BUF_SIZE = 256;
        char m_customValueBuf[BUF_SIZE] = "";
        const void* m_activeModalParam = nullptr;
        bool m_modalOpenRequested = false;
    };

    REGISTER_FIELD_EDITOR(ParamType::FloatVec, FloatVecFieldEditor)

} // namespace VKIntox
