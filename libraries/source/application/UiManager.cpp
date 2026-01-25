#include "application/UiManager.h"

#include "application/NativeView.h"
#include "core/Exception.h"
#include "core/FileManager.h"
#include "core/ThreadManager.h"

#include <IconsFontAwesome7.h>
#include <imgui.h>

namespace sparkle
{
static void SetupStyle()
{
    auto &io = ImGui::GetIO();

    {
        auto *file_manager = FileManager::GetNativeFileManager();

        // Load main font
        auto font_data = file_manager->Read(Path::Resource("fonts/Roboto-Medium.ttf"));
        ASSERT(!font_data.empty());

        ImFontConfig font_config;
        font_config.FontDataOwnedByAtlas = false;
        io.FontDefault =
            io.Fonts->AddFontFromMemoryTTF(font_data.data(), static_cast<int>(font_data.size()), 20, &font_config);

        // Merge Font Awesome icons into the main font
        auto icon_font_data = file_manager->Read(Path::Resource("fonts/FontAwesome7-Solid.otf"));
        ASSERT(!icon_font_data.empty());

        ImFontConfig icon_config;
        icon_config.FontDataOwnedByAtlas = false;
        icon_config.MergeMode = true;
        icon_config.GlyphMinAdvanceX = 20.0f; // Make icons monospaced to match main font size
        icon_config.PixelSnapH = true;
        static const ImWchar IconRanges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
        io.Fonts->AddFontFromMemoryTTF(icon_font_data.data(), static_cast<int>(icon_font_data.size()), 20, &icon_config,
                                       IconRanges);

        io.Fonts->Build();
    }

    auto &style = ImGui::GetStyle();

    style.WindowPadding = ImVec2(20, 8);
    style.WindowRounding = 10.0f;
    style.FramePadding = ImVec2(5, 5);
    style.FrameRounding = 12.0f; // Make all elements (checkboxes, etc) circles
    style.ItemSpacing = ImVec2(20.f, 10.f);
    style.ScrollbarSize = 15.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize = 20.0f; // Make grab a circle
    style.GrabRounding = 12.0f;
    style.PopupRounding = 7.f;
    style.Alpha = 1.0f;

    style.ScaleAllSizes(2.0f);

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.90f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.90f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.19f, 0.19f, 0.19f, 0.92f);
    colors[ImGuiCol_Border] = ImVec4(0.16f, 0.16f, 0.16f, 0.9f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.24f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.19f, 0.19f, 0.9f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.23f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.54f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.34f, 0.34f, 0.34f, 0.54f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 0.54f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.54f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.33f, 0.67f, 0.86f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.34f, 0.34f, 0.34f, 0.54f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.54f);
    colors[ImGuiCol_Button] = ImVec4(0.905f, 0.905f, 0.905f, 0.54f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.19f, 0.19f, 0.54f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.22f, 0.23f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.00f, 0.00f, 0.00f, 0.36f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.22f, 0.23f, 0.33f);
    colors[ImGuiCol_Separator] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.03f, 0.03f, 0.03f, 0.52f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.20f, 0.20f, 0.20f, 0.36f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.20f, 0.22f, 0.23f, 1.00f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.33f, 0.67f, 0.86f, 1.00f);
    colors[ImGuiCol_NavHighlight] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 0.00f, 0.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(1.00f, 0.00f, 0.00f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(1.00f, 0.00f, 0.00f, 0.35f);
}

static ImDrawData *CloneDrawData(const ImDrawData *original)
{
    if (!original)
    {
        return nullptr;
    }

    auto *clone = IM_NEW(ImDrawData);
    *clone = *original;

    if (original->CmdListsCount > 0)
    {
        for (int i = 0; i < original->CmdListsCount; i++)
        {
            clone->CmdLists[i] = original->CmdLists[i]->CloneOutput();
        }
    }

    return clone;
}

static void FreeDrawData(ImDrawData *draw_data)
{
    if (!draw_data)
    {
        return;
    }

    for (int i = 0; i < draw_data->CmdListsCount; i++)
    {
        IM_DELETE(draw_data->CmdLists[i]);
    }

    IM_DELETE(draw_data);
}

UiManager::UiManager(NativeView *native_view) : native_view_(native_view)
{
    ASSERT(ThreadManager::IsInMainThread());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    io_ = &ImGui::GetIO();
#if FRAMEWORK_ANDROID || FRAMEWORK_IOS
    io_->ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
#endif

    SetupStyle();

    native_view_->InitUiSystem();

    std::ranges::fill(draw_data_per_frame_, nullptr);

    // after this point, RHIUiHandler will call Init() in render thread
}

UiManager::~UiManager()
{
    ASSERT(io_ == nullptr);
    Shutdown();
}

void UiManager::Render()
{
    ASSERT(io_);

    ASSERT(ThreadManager::IsInMainThread());

    if (pending_windows_to_draw_.empty())
    {
        draw_data_per_frame_[main_thread_context_index_] = nullptr;
    }
    else
    {
        native_view_->TickUiSystem();

        ImGui::NewFrame();

        for (auto &window : pending_windows_to_draw_)
        {
            window.ui_generator();
        }

        // render does not actually happen at this point.
        ImGui::Render();

        FreeDrawData(draw_data_per_frame_[main_thread_context_index_]);

        auto *draw_data_clone = CloneDrawData(ImGui::GetDrawData());
        draw_data_per_frame_[main_thread_context_index_] = draw_data_clone;

        pending_windows_to_draw_.clear();
    }

    main_thread_context_index_ = (main_thread_context_index_ + 1) % draw_data_per_frame_.size();

    // after this point, RHIUiHandler will call BeginFrame() and Render() in render thread
}

void UiManager::BeginRenderThread()
{
    ASSERT(ThreadManager::IsInRenderThread());

    ImGui::GetIO().UserData = draw_data_per_frame_[render_thread_context_index_];

    render_thread_context_index_ = (render_thread_context_index_ + 1) % draw_data_per_frame_.size();
}

bool UiManager::HasDataToDraw()
{
    ASSERT(ThreadManager::IsInRenderThread());

    return ImGui::GetIO().UserData != nullptr;
}

void UiManager::RequestWindowDraw(CustomUiWindow &&window)
{
    pending_windows_to_draw_.emplace_back(std::move(window));
}

void UiManager::Shutdown()
{
    ASSERT(ThreadManager::IsInMainThread());

    if (!io_)
    {
        return;
    }

    // before this point, RHIUiHandler should have called Shutdown() in render thread
    native_view_->ShutdownUiSystem();

    std::ranges::for_each(draw_data_per_frame_, FreeDrawData);

    ImGui::DestroyContext();

    io_ = nullptr;
}

bool UiManager::IsHandlingMouseEvent()
{
    if (!io_)
    {
        return false;
    }
    return io_->WantCaptureMouse;
}

bool UiManager::IsHanldingKeyboradEvent()
{
    if (!io_)
    {
        return false;
    }
    return io_->WantCaptureKeyboard;
}
} // namespace sparkle
