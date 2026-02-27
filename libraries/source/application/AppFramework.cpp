#include "application/AppFramework.h"

#include "application/NativeKeyboard.h"
#include "application/NativeView.h"
#include "application/RenderFramework.h"
#include "application/SessionManager.h"
#include "application/UiManager.h"
#include "core/ConfigManager.h"
#include "core/CoreStates.h"
#include "core/Event.h"
#include "core/FileManager.h"
#include "core/Path.h"
#include "core/Profiler.h"
#include "core/task/TaskManager.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/camera/OrbitCameraComponent.h"
#include "scene/material/MaterialManager.h"

#if ENABLE_TEST_CASES
#include "application/TestCase.h"
#endif

#include <IconsFontAwesome7.h>
#include <imgui.h>
#include <imgui_internal.h>

namespace sparkle
{
constexpr float LogInterval = 1.f;

static void ClearScreenshots()
{
    Log(Info, "Clearing screenshots");

    auto *fm = FileManager::GetNativeFileManager();
    auto screenshot_dir = Path::External("screenshots");

    if (!fm->IsDirectory(screenshot_dir))
    {
        return;
    }

    for (const auto &entry : fm->ListDirectory(screenshot_dir))
    {
        if (fm->IsRegularFile(entry))
        {
            Log(Info, "Removing {}", entry.path.filename().string());
            fm->Remove(entry);
        }
    }
}

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

    session_manager_ = std::make_unique<SessionManager>();
    session_manager_->SetLoadLastSession(app_config_.load_last_session);
    session_manager_->LoadLastSessionIfRequested();

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

    if (render_config_.clear_screenshots)
    {
        ClearScreenshots();
    }

    return true;
}

bool AppFramework::Init()
{
    ASSERT_F(core_initialized_, "Core is not initialized. Call InitCore first");
    ASSERT_F(view_, "No valid native view. Call SetNativeView first");

    if (app_config_.headless)
    {
        if (app_config_.platform == AppConfig::NativePlatform::iOS ||
            app_config_.platform == AppConfig::NativePlatform::Android)
        {
            Log(Error, "Headless mode is not supported on mobile platforms.");
            return false;
        }
#if !FRAMEWORK_GLFW && !FRAMEWORK_MACOS
        Log(Error, "Headless mode is currently supported only on GLFW and macOS frameworks.");
        return false;
#endif
    }

    {
        PROFILE_SCOPE_LOG("Init native view");

        view_->InitGUI(this);

        view_->SetTitle(app_config_.app_name);
    }

    {
        PROFILE_SCOPE_LOG("Init GUI");

        if (!view_->IsHeadless())
        {
            ui_manager_ = std::make_unique<UiManager>(view_);
        }
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
        render_framework_->StartRenderThread(render_config_);
    }
    else
    {
        Log(Info, "Render thread disabled. All rendering will happen on main thread.");
    }

    renderer_created_subscription_ =
        render_framework_->ListenRendererCreatedEvent().Subscribe([this]() { renderer_ready_ = true; });

    scene_load_task_ = SceneManager::LoadScene(main_scene_.get(), Path::Resource(app_config_.scene),
                                               app_config_.default_skybox, render_config_.IsRaterizationMode());

    if (session_manager_)
    {
        session_manager_->ApplyCamera(GetMainCamera());
    }

    Log(Info, "Default scene loading task dispatched");

    frame_timer_.Reset();

    CoreStates::Instance().SetAppState(CoreStates::AppState::MainLoop);

    initialized_ = true;

    Log(Info, "Init success. Main loop started");

#if ENABLE_TEST_CASES
    if (!app_config_.test_case.empty())
    {
        test_case_ = TestCaseRegistry::Create(app_config_.test_case);
        if (!test_case_)
        {
            return false;
        }
        Log(Info, "Test case '{}' loaded", app_config_.test_case);
    }
#endif

    return true;
}

struct VerticalIconTab
{
    const char *icon; // Font Awesome icon string
    std::function<void()> draw;
};

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
                {"App", &app_config_},
                {"Render", &render_config_},
                {"RHI", &rhi_config_},
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

    if (scene_load_task_ && scene_load_task_->IsReady())
    {
        scene_load_task_.reset();
        scene_file_loaded_ = true;
        Log(Info, "Scene file loaded");
    }

    if (scene_file_loaded_ && !scene_async_tasks_completed_ && !main_scene_->HasPendingAsyncTasks())
    {
        scene_async_tasks_completed_ = true;
        Log(Info, "Scene async tasks completed");
        render_framework_->NotifySceneLoaded();
    }

#if ENABLE_TEST_CASES
    if (test_case_ && scene_async_tasks_completed_)
    {
        const auto result = test_case_->Tick(*this);
        if (result == TestCase::Result::Pass)
        {
            Log(Info, "TestCase PASS");
            RequestExit();
        }
        else if (result == TestCase::Result::Fail)
        {
            Log(Error, "TestCase FAIL");
            exit_code_ = 1;
            RequestExit();
        }
    }
#endif

    // Camera animation: initialize once after scene is ready, then apply each frame
    if (scene_async_tasks_completed_ && !camera_animator_initialized_)
    {
        auto path_type = CameraAnimator::FromString(render_config_.camera_animation);
        if (path_type != CameraAnimator::PathType::kNone)
        {
            if (auto *camera = dynamic_cast<OrbitCameraComponent *>(GetMainCamera()))
            {
                CameraAnimator::OrbitPose initial{};
                initial.center = camera->GetCenter();
                initial.radius = camera->GetRadius();
                initial.pitch = camera->GetPitch();
                initial.yaw = camera->GetYaw();
                uint32_t anim_frames = render_config_.camera_animation_frames;
                if (anim_frames == 0)
                {
                    anim_frames = render_config_.max_sample_per_pixel / 2;
                }
                camera_animator_.Setup(path_type, anim_frames, initial);
                Log(Info, "CameraAnimator active: {} for {} frames (max_spp={})",
                    render_config_.camera_animation, anim_frames, render_config_.max_sample_per_pixel);
            }
        }
        camera_animator_initialized_ = true;
    }

    if (camera_animator_.IsActive() && !camera_animator_.IsDone(static_cast<uint32_t>(frame_number_)))
    {
        auto pose = camera_animator_.GetPose(static_cast<uint32_t>(frame_number_));
        if (auto *camera = dynamic_cast<OrbitCameraComponent *>(GetMainCamera()))
        {
            camera->Setup(pose.center, pose.radius, pose.pitch, pose.yaw);
        }
    }

    {
        PROFILE_SCOPE("MainLoop tick scene");

        main_scene_->Tick();
        main_scene_->ProcessChange();
    }

    {
        PROFILE_SCOPE("MainLoop render ui");

        if (ui_manager_)
        {
            DrawUi();
            ui_manager_->Render();
        }
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
    ui_mouse_sequence_active_ = false;
    last_x_ = -1;
    last_y_ = -1;
}

bool AppFramework::ShouldConsumeSceneMouseInput(MouseInputType input_type, double x, double y,
                                                bool has_pointer_position)
{
    if (!ui_manager_)
    {
        return false;
    }

    const bool pointer_over_ui =
        has_pointer_position && ui_manager_->IsPointerOverUi(static_cast<float>(x), static_cast<float>(y));
    const bool ui_wants_mouse = ui_manager_->IsHandlingMouseEvent();
    const bool ui_interacting = ui_wants_mouse || pointer_over_ui || ui_mouse_sequence_active_;

    if (input_type == MouseInputType::Press)
    {
        ui_mouse_sequence_active_ = ui_wants_mouse || pointer_over_ui;
        return ui_mouse_sequence_active_;
    }

    if (input_type == MouseInputType::Release)
    {
        const bool consume_release = ui_interacting;
        ui_mouse_sequence_active_ = false;
        return consume_release;
    }

    return ui_interacting;
}

void AppFramework::CancelScenePointerInteraction()
{
    if (!current_pressing_)
    {
        return;
    }

    if (auto *camera = GetMainCamera())
    {
        camera->OnPointerUp();
    }

    current_pressing_ = false;
}

void AppFramework::CursorPositionCallback(double xPos, double yPos)
{
    const auto last_point = GetLastClickPoint();
    SetLastClickPoint(static_cast<float>(xPos), static_cast<float>(yPos));

    if (ShouldConsumeSceneMouseInput(MouseInputType::Move, xPos, yPos, true))
    {
        CancelScenePointerInteraction();
        return;
    }

    auto *camera = GetMainCamera();
    if (!camera)
    {
        return;
    }

    if (current_pressing_ && last_point.x() >= 0.f && last_point.y() >= 0.f)
    {
        camera->OnPointerMove(static_cast<float>(yPos) - last_point.y(), last_point.x() - static_cast<float>(xPos));
    }
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

void AppFramework::ScrollCallback(double /*xoffset*/, double yoffset)
{
    const bool has_pointer_position = last_x_ >= 0.f && last_y_ >= 0.f;
    if (ShouldConsumeSceneMouseInput(MouseInputType::Scroll, static_cast<double>(last_x_), static_cast<double>(last_y_),
                                     has_pointer_position))
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
    MouseInputType input_type = MouseInputType::Move;
    if (button == ClickButton::Primary_Left)
    {
        input_type = action == KeyAction::Press ? MouseInputType::Press : MouseInputType::Release;
    }

    const bool has_pointer_position = last_x_ >= 0.f && last_y_ >= 0.f;
    if (ShouldConsumeSceneMouseInput(input_type, static_cast<double>(last_x_), static_cast<double>(last_y_),
                                     has_pointer_position))
    {
        CancelScenePointerInteraction();
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
            if (!current_pressing_)
            {
                return;
            }
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
            show_control_panel_ = !show_control_panel_;

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
            SceneManager::RemoveLastDebugSphere(main_scene_.get());
        }
        break;
    }
    default:
        break;
    }
}

void AppFramework::RequestExit()
{
    // TODO(tqjxlm): handle pending async tasks

    CoreStates::Instance().SetAppState(CoreStates::AppState::Exiting);
}

void AppFramework::CaptureNextFrames(int count)
{
    rhi_->CaptureNextFrames(count);
}

std::shared_ptr<ScreenshotRequest> AppFramework::RequestTakeScreenshot(const std::string &name)
{
    return render_framework_->RequestTakeScreenshot(name);
}

bool AppFramework::IsReadyForAutoScreenshot() const
{
    return render_framework_->IsReadyForAutoScreenshot();
}

CameraComponent *AppFramework::GetMainCamera() const
{
    return main_scene_->GetMainCamera();
}
} // namespace sparkle
