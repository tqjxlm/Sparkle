#include "application/AppFramework.h"

#include "application/RenderFramework.h"
#include "application/SessionManager.h"
#include "application/UiManager.h"
#include "core/ConfigManager.h"
#include "core/CoreStates.h"
#include "core/GitVersion.h"
#include "renderer/denoiser/DenoiserConfig.h"
#include "renderer/nrd/NrdConfig.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"

#include <IconsFontAwesome7.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <functional>
#include <vector>

namespace
{
struct VerticalIconTab
{
    const char *icon; // Font Awesome icon string
    std::function<void()> draw;
};
} // namespace

namespace sparkle
{
static void DrawVerticalIconTabs(const std::vector<VerticalIconTab> &tabs, unsigned &current_tab)
{
    // vertical tab bar
    {
        const ImVec2 icon_size{80.f, 40.f};
        const float bar_width = icon_size.x;

        ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing, 10.f);

        ImGui::BeginChild("icon_bar", ImVec2(bar_width, 0), 0,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        for (unsigned i = 0u; i < tabs.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            bool selected = (current_tab == i);

            // highlight selected tab
            if (selected)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.44f, 0.60f, 1.f)),
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.49f, 0.68f, 1.f)),
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.80f, 0.80f, 0.90f, 1.f));
            }

            bool pressed = ImGui::Button(tabs[i].icon, icon_size);
            if (pressed)
            {
                current_tab = i;
            }

            if (selected)
            {
                ImGui::PopStyleColor(3);
            }

            ImGui::PopID();
        }

        ImGui::EndChild();

        ImGui::PopStyleVar(1);
    }

    // vertical separator
    {
        ImGui::SameLine();

        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    }

    // content page
    {
        ImGui::SameLine();

        ImGui::BeginChild("page");
        tabs[current_tab].draw();
        ImGui::EndChild();
    }
}

void AppFramework::DrawUi()
{
    if (!ui_manager_)
    {
        return;
    }

    if (!renderer_ready_)
    {
        // imgui depends on rhi context, so we need to wait for it to be created
        return;
    }

    if (show_control_panel_)
    {
        ui_manager_->RequestWindowDraw({[this]() {
            static std::vector<std::pair<const char *, ConfigCollection *>> configs{
                {"App", &app_config_},      {"Render", &render_config_},
                {"RHI", &rhi_config_},      {"Denoiser", &DenoiserConfig::Get()},
                {"NRD", &NrdConfig::Get()},
            };

            float font_size = ImGui::GetFontSize();

            const ImGuiViewport *main_viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + 20, main_viewport->WorkPos.y + 20),
                                    ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(font_size * 30, font_size * 30), ImGuiCond_Always);

            ImGuiWindowFlags window_flags = 0;
            window_flags |= ImGuiWindowFlags_NoDecoration;
            window_flags |= ImGuiWindowFlags_NoMove;

            ImGui::Begin("Control Panel", nullptr, window_flags);

            static unsigned current_tab = 0;
            static std::vector<VerticalIconTab> tabs{
                {.icon = ICON_FA_FOLDER,
                 .draw =
                     [this]() {
                         SceneManager::DrawUi(main_scene_.get(), app_config_.default_skybox,
                                              render_config_.IsRaterizationMode());
                     }},
                {.icon = ICON_FA_ARROW_ROTATE_LEFT,
                 .draw =
                     [this]() {
                         session_manager_->DrawUi(main_scene_.get(), app_config_.default_skybox,
                                                  render_config_.IsRaterizationMode());
                     }},
                {.icon = ICON_FA_CAMERA, .draw = [this]() { render_framework_->DrawUi(); }},
                {.icon = ICON_FA_GEAR, .draw = [=]() { ConfigManager::DrawUi(configs); }}};
            DrawVerticalIconTabs(tabs, current_tab);

            ImGui::End();
        }});
    }

    if (app_config_.show_screen_log)
    {
        logger_->DrawUi(ui_manager_.get());
    }
}
} // namespace sparkle
