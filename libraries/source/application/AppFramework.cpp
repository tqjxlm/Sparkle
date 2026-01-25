#include "application/AppFramework.h"

#include "application/NativeKeyboard.h"
#include "application/NativeView.h"
#include "application/RenderFramework.h"
#include "application/UiManager.h"
#include "core/ConfigManager.h"
#include "core/CoreStates.h"
#include "core/Event.h"
#include "core/FileManager.h"
#include "core/Profiler.h"
#include "core/task/TaskManager.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/material/MaterialManager.h"

#include <IconsFontAwesome7.h>
#include <imgui.h>

#include <format>
#include <imgui_internal.h>

namespace sparkle
{
constexpr float LogInterval = 1.f;

AppFramework::AppFramework()
    : frame_rate_monitor_(LogInterval, false, [this](float delta_time) { MeasurePerformance(delta_time); })
{
    pending_tasks_ = std::make_shared<ThreadTaskQueue>();
}

AppFramework::~AppFramework()
{
    Cleanup();
}

bool AppFramework::InitCore(int argc, const char *const argv[])
{
    CoreStates::Instance().SetAppState(CoreStates::AppState::Init);

    logger_ = std::make_unique<Logger>();

    // after this point, we can use LN_LOG
    Log(Info, "Program started");

    PROFILE_SCOPE_LOG("Init core");

    sparkle::ThreadManager::RegisterMainThread();

    ConfigManager &config_manager = ConfigManager::Instance();
    config_manager.SetArgs(argc, argv);
    config_manager.LoadAll();

    app_config_.Init();
    render_config_.Init();
    rhi_config_.Init();

    task_manager_ = std::make_unique<TaskManager>(app_config_.max_threads);
    TaskDispatcher::Instance().RegisterTaskQueue(pending_tasks_, ThreadName::Main);

#if ENABLE_PROFILER
    Profiler::RegisterThreadForProfiling("Main");
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    Log(Info, "Asan is enabled");
#endif
#endif

    Logger::LogToScreen("Usage", "Double click to toggle config");

    core_initialized_ = true;

    return true;
}

bool AppFramework::Init()
{
    ASSERT_F(core_initialized_, "Core is not initialized. Call InitCore first");
    ASSERT_F(view_, "No valid native view. Call SetNativeView first");

    {
        PROFILE_SCOPE_LOG("Init native view");

        view_->InitGUI(this);

        view_->SetTitle(app_config_.app_name);
    }

    {
        PROFILE_SCOPE_LOG("Init GUI");

        ui_manager_ = std::make_unique<UiManager>(view_);
    }

    {
        PROFILE_SCOPE_LOG("Init RHI");

        rhi_ = RHIContext::CreateRHI(rhi_config_);

        std::string rhi_error;
        if (!rhi_ || !rhi_->InitRHI(view_, rhi_error))
        {
            Log(Error, "Failed to init rhi: {}", rhi_error);
            return false;
        }

        rhi_->InitRenderResources();
    }

    // now that we have a valid rhi and native view, allow render config to validate against them.
    render_config_.SetupBackend(rhi_.get(), view_);

    material_manager_ = MaterialManager::CreateInstance();

    main_scene_ = std::make_unique<Scene>();

    render_framework_ = std::make_unique<RenderFramework>(view_, rhi_.get(), ui_manager_.get(), main_scene_.get());
    if (app_config_.render_thread)
    {
        render_framework_->StartRenderThread();
    }
    else
    {
        Log(Info, "Render thread disabled. All rendering will happen on main thread.");
    }

    renderer_created_subscription_ =
        render_framework_->ListenRendererCreatedEvent().Subscribe([this]() { renderer_ready_ = true; });

    SceneManager::LoadScene(main_scene_.get(), Path::Resource(app_config_.scene), app_config_.default_skybox,
                            render_config_.IsRaterizationMode());

    Log(Info, "Default scene loading task dispatched");

    frame_timer_.Reset();

    CoreStates::Instance().SetAppState(CoreStates::AppState::MainLoop);

    initialized_ = true;

    Log(Info, "Init success. Main loop started");

    return true;
}

struct VerticalIconTab
{
    const char *icon; // Font Awesome icon string
    std::function<void()> draw;
};

static void DrawVerticalIconTabs(const std::vector<VerticalIconTab> &tabs, unsigned &current_tab)
{
    const float button_size = 40.f;             // square icon buttons
    const float bar_width = button_size + 10.f; // icon strip width
    const ImVec2 icon_size{button_size, button_size};

    // tighten horizontal spacing
    ImGuiStyle &style = ImGui::GetStyle();
    float saved_spacing_x = style.ItemSpacing.x;

    // gap between page & bar
    style.ItemSpacing.x = 10.f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));

    ImGui::BeginChild("icon_bar", ImVec2(bar_width, 0), 0,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    for (unsigned i = 0u; i < tabs.size(); ++i)
    {
        ImGui::PushID(i);
        bool sel = (current_tab == i);

        // highlight selected tab
        if (sel)
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

        if (sel)
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);

    ImGui::SameLine();

    ImGui::BeginChild("page");
    tabs[current_tab].draw();
    ImGui::EndChild();

    style.ItemSpacing.x = saved_spacing_x;

    ImGui::PopStyleColor(1);
}

void AppFramework::GenerateBuiltinUi()
{
    if (!renderer_ready_)
    {
        // imgui depends on rhi context, so we need to wait for it to be created
        return;
    }

    if (show_settngs_)
    {
        ui_manager_->RequestWindowDraw({[this]() {
            static std::vector<std::pair<const char *, ConfigCollection *>> configs{
                {"App", &app_config_},
                {"Render", &render_config_},
                {"RHI", &rhi_config_},
            };

            static unsigned current_tab = 0;
            static std::vector<VerticalIconTab> tabs{
                {.icon = ICON_FA_FOLDER,
                 .draw =
                     [this]() {
                         SceneManager::DrawUi(main_scene_.get(), app_config_.default_skybox,
                                              render_config_.IsRaterizationMode());
                     }},
                {.icon = ICON_FA_GEAR, .draw = [=]() { ConfigManager::DrawUi(configs); }}};

            float font_size = ImGui::GetFontSize();

            const ImGuiViewport *main_viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + 20, main_viewport->WorkPos.y + 20),
                                    ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(font_size * 40, font_size * 30), ImGuiCond_Always);

            ImGuiWindowFlags window_flags = 0;
            window_flags |= ImGuiWindowFlags_NoResize;
            window_flags |= ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoCollapse;

            ImGui::Begin("Control Panel", nullptr, window_flags);

            DrawVerticalIconTabs(tabs, current_tab);

            ImGui::End();
        }});
    }

    if (app_config_.show_screen_log)
    {
        logger_->DrawUi(ui_manager_.get());
    }
}

bool AppFramework::MainLoop()
{
    if (CoreStates::IsExiting())
    {
        return false;
    }

    static const char *main_thread_name = "main thread";

    PROFILE_FRAME_START(main_thread_name);

    Timer main_thread_timer;

    // push new render config to render thread. it will not take effect until the next frame on render thread.
    TaskManager::RunInRenderThread([this, frame_number = frame_number_, render_config = render_config_]() {
        render_framework_->NewFrame(frame_number, render_config);
    });

    {
        PROFILE_SCOPE("MainLoop ConsumeThreadTasks");

        pending_tasks_->RunAll();
    }

    {
        PROFILE_SCOPE("MainLoop tick native view");

        // TODO(tqjxlm): use an event system instead of direct callbacks
        view_->Tick();

        if (view_->ShouldClose())
        {
            return true;
        }
    }

    {
        PROFILE_SCOPE("MainLoop tick scene");

        main_scene_->Tick();
        main_scene_->ProcessChange();
    }

    {
        PROFILE_SCOPE("MainLoop render ui");

        GenerateBuiltinUi();
        ui_manager_->Render();
    }

    AdvanceFrame(static_cast<float>(main_thread_timer.ElapsedMicroSecond()) * 1e-3f);

    // it will block the main thread when the render thread task queue is full
    render_framework_->PushRenderTasks();

    if (!app_config_.render_thread)
    {
        render_framework_->RenderLoop();
    }

    PROFILE_FRAME_END(main_thread_name);

    return true;
}

void AppFramework::AdvanceFrame(float main_thread_time)
{
    delta_time_ = frame_timer_.ElapsedSecond();
    frame_timer_.Reset();

    last_second_main_thread_time_ += main_thread_time;

    frame_number_++;

    frame_rate_monitor_.Tick();
}

void AppFramework::MeasurePerformance(float delta_time)
{
    static uint64_t last_frame_number = 0;
    const auto last_second_frame_cnt = static_cast<float>(frame_number_ - last_frame_number);
    last_frame_number = frame_number_;

    // delta_time should be something very close to 1.0 seconds
    const float last_second_average_frame_time = delta_time / last_second_frame_cnt;
    const float last_second_average_main_thread_time = last_second_main_thread_time_ / last_second_frame_cnt;

    Logger::LogToScreen("FPS", std::format("FPS: {:.1f}", 1.f / last_second_average_frame_time));
    Logger::LogToScreen("Frame", std::format("Frame: {:.1f} ms", last_second_average_frame_time * 1000.f));
    Logger::LogToScreen("MainThread", std::format("Main thread: {:.1f} ms", last_second_average_main_thread_time));

    last_second_main_thread_time_ = 0;
}

void AppFramework::Cleanup()
{
    if (!initialized_)
    {
        return;
    }

    Log(Info, "AppFramework::Cleanup");

    {
        Log(Info, "Clean up render framework");

        if (app_config_.render_thread)
        {
            // wait for all remaining render tasks to be executed
            render_framework_->StopRenderThread();
        }
        else
        {
            rhi_->WaitForDeviceIdle();
        }

        render_framework_ = nullptr;

        // after this point, all render thread tasks will run immediately on main thread
        ThreadManager::RegisterRenderThread();
    }

    {
        Log(Debug, "Clean up resources");

        material_manager_->Destroy();
        material_manager_ = nullptr;

        main_scene_ = nullptr;
    }

    {
        Log(Debug, "Clean up core components");

        if (view_)
        {
            view_->Cleanup();
        }

        task_manager_ = nullptr;

        rhi_->Cleanup();

        rhi_ = nullptr;

        if (ui_manager_)
        {
            ui_manager_->Shutdown();
        }
    }

    initialized_ = false;

    core_initialized_ = false;

    FileManager::DestroyNativeFileManager();

    Log(Info, "App exit gracefully.");

    logger_ = nullptr;

    // no more logging should happen after this point
}

void AppFramework::DebugNextFrame()
{
    auto scale = view_->GetWindowScale();
    TaskManager::RunInRenderThread([this, scale]() {
        render_framework_->SetDebugPoint(last_x_ * scale.x(),
                                         static_cast<float>(render_config_.image_height) - last_y_ * scale.y());
    });
}

void AppFramework::ResetInputEvents()
{
    current_pressing_ = false;
    last_x_ = -1;
    last_y_ = -1;
}

void AppFramework::CursorPositionCallback(double xPos, double yPos)
{
    if (ui_manager_ && ui_manager_->IsHandlingMouseEvent())
    {
        return;
    }

    auto *camera = GetMainCamera();
    if (!camera)
    {
        return;
    }

    if (current_pressing_)
    {
        auto last_point = GetLastClickPoint();
        camera->OnPointerMove(static_cast<float>(yPos) - last_point.y(), last_point.x() - static_cast<float>(xPos));
    }
    SetLastClickPoint(static_cast<float>(xPos), static_cast<float>(yPos));
}

void AppFramework::FrameBufferResizeCallback(int width, int height) const
{
    TaskManager::RunInRenderThread([this, width, height]() {
        if (render_framework_)
        {
            render_framework_->OnFrameBufferResize(width, height);
        }
    });
}

void AppFramework::ScrollCallback(double /*xoffset*/, double yoffset) const
{
    if (ui_manager_ && ui_manager_->IsHandlingMouseEvent())
    {
        return;
    }

    auto *camera = GetMainCamera();
    if (!camera)
    {
        return;
    }

    camera->OnScroll(static_cast<float>(app_config_.platform == AppConfig::NativePlatform::MacOS ? -yoffset : yoffset));
}

void AppFramework::MouseButtonCallback(ClickButton button, KeyAction action, uint32_t mods)
{
    if (ui_manager_ && ui_manager_->IsHandlingMouseEvent())
    {
        return;
    }

    auto *camera = GetMainCamera();
    if (!camera)
    {
        return;
    }

    if (button == ClickButton::Primary_Left)
    {
        if (action == KeyAction::Release)
        {
            ASSERT(current_pressing_ == true);
            current_pressing_ = false;
            camera->OnPointerUp();

            static const unsigned ClickThresholdMS = 200;
            if (click_timer_.ElapsedMilliSecond() < ClickThresholdMS)
            {
                ClickCallback();
            }
        }
        else if (action == KeyAction::Press)
        {
            if (mods & static_cast<uint32_t>(KeyboardModifier::Control))
            {
                DebugNextFrame();
                return;
            }
            current_pressing_ = true;
            camera->OnPointerDown();

            click_timer_.Reset();
        }
    }

    if (button == ClickButton::Secondary_Right)
    {
        if (action == KeyAction::Press)
        {
            DebugNextFrame();
            return;
        }
    }
}

void AppFramework::ClickCallback()
{
    static const unsigned DoubleClickThresholdMS = 300;
    static const unsigned DoubleClickCooldownMS = 300;

    if (double_click_timer_.ElapsedMilliSecond() < DoubleClickThresholdMS)
    {
        if (double_click_cooldown_.ElapsedMilliSecond() > DoubleClickCooldownMS)
        {
            show_settngs_ = !show_settngs_;

            double_click_cooldown_.Reset();
        }
    }

    double_click_timer_.Reset();
}

void AppFramework::KeyboardCallback(int key, KeyAction action, bool shift_on) const
{
    if (ui_manager_ && ui_manager_->IsHanldingKeyboradEvent())
    {
        return;
    }

    auto *camera = GetMainCamera();
    if (!camera)
    {
        return;
    }

    switch (static_cast<NativeKeyboard>(key))
    {
    case NativeKeyboard::KEY_ESCAPE: {
        if (action == KeyAction::Release)
        {
            RequestExit();
        }
        break;
    }
    case NativeKeyboard::KEY_UP: {
        if (action == KeyAction::Release)
        {
            camera->SetAperture(camera->GetAttribute().aperture + 1.f);
        }
        break;
    }
    case NativeKeyboard::KEY_DOWN: {
        if (action == KeyAction::Release)
        {
            camera->SetAperture(camera->GetAttribute().aperture - 1.f);
        }
        break;
    }
    case NativeKeyboard::KEY_P: {
        if (action == KeyAction::Release)
        {
            camera->PrintPosture();
        }
        break;
    }
    case NativeKeyboard::KEY_KP_ADD: {
        if (action == KeyAction::Release)
        {
            Log(Debug, "Add debug sphere");
            SceneManager::GenerateRandomSpheres(*main_scene_, 1);
        }
        break;
    }
    case NativeKeyboard::KEY_EQUAL: {
        if (shift_on)
        {
            // it is actually '+'
            if (action == KeyAction::Release)
            {
                Log(Debug, "Add debug sphere");
                SceneManager::GenerateRandomSpheres(*main_scene_, 1);
            }
        }
        break;
    }
    case NativeKeyboard::KEY_MINUS: {
        if (action == KeyAction::Release)
        {
            Log(Debug, "Remove debug sphere");
            SceneManager::RemoveLastNode(main_scene_.get());
        }
        break;
    }
    default:
        break;
    }
}

void AppFramework::RequestExit()
{
    CoreStates::Instance().SetAppState(CoreStates::AppState::Exiting);
}

void AppFramework::CaptureNextFrames(int count)
{
    rhi_->CaptureNextFrames(count);
}

CameraComponent *AppFramework::GetMainCamera() const
{
    return main_scene_->GetMainCamera();
}
} // namespace sparkle
