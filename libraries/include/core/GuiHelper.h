#pragma once

#include <imgui.h>

#include <string>

namespace sparkle::gui_helper
{
inline std::string LabelPrefix(const std::string &label)
{
    float width = ImGui::CalcItemWidth();

    float x = ImGui::GetCursorPosX();

    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label.c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosX(x + width * 0.5f + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(-1);

    std::string label_id = "##";
    label_id += label;

    return label_id;
}
} // namespace sparkle::gui_helper
