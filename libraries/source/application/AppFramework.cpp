#include "application/AppFramework.h"

#include "application/CookPipeline.h"
#include "application/InputManager.h"
#include "application/NativeView.h"
#include "application/RenderFramework.h"
#include "application/SessionManager.h"
#include "application/UiManager.h"
#include "core/ConfigManager.h"
#include "core/CoreStates.h"
#include "core/Event.h"
#include "core/FileManager.h"
#include "core/GitVersion.h"
#include "core/Path.h"
#include "core/Profiler.h"
#include "core/task/TaskManager.h"
#include "io/CookTargets.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/material/MaterialManager.h"

#if ENABLE_TEST_CASES
#include "application/TestCase.h"
#endif

#include <algorithm>
#include <cstring>
#include <iterator>

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

    // parsed from raw argv: the logger exists before the config system, and desktop builds
    // are the only ones whose log location a caller (e.g. the cook stage) can direct
    std::string dedicated_log_path;
#if FRAMEWORK_GLFW || FRAMEWORK_MACOS
    for (int i = 1; i + 1 < argc; i++)
    {
        if (std::strcmp(argv[i], "--log_path") == 0)
        {
            dedicated_log_path = argv[i + 1];
        }
    }
#endif

    logger_ = std::make_unique<Logger>(dedicated_log_path);

    // after this point, we can use LN_LOG
    Log(Info, "Program started");
    Log(Info, "Git version: {} ({})", GitCommit, GitBranch);

    PROFILE_SCOPE_LOG("Init core");

    sparkle::ThreadManager::RegisterMainThread();

    ConfigManager &config_manager = ConfigManager::Instance();
    config_manager.SetArgs(argc, argv);
    config_manager.LoadAll();

    app_config_.Init();

    if (app_config_.cook_mode)
    {
        app_config_.headless = true;
    }

#if ENABLE_TEST_CASES
    if (!app_config_.test_case.empty())
    {
        test_case_ = TestCaseRegistry::Create(app_config_.test_case);
        if (!test_case_)
        {
            return false;
        }

        test_case_->EnforceConfigs();
        Log(Info, "Test case '{}' loaded", test_case_->GetName());
    }
#endif

    render_config_.Init();
    rhi_config_.Init();

    session_manager_ = std::make_unique<SessionManager>();
    session_manager_->SetLoadLastSession(app_config_.load_last_session);
    session_manager_->LoadLastSessionIfRequested();

    // cook mode drives the RHI inline from main and runs no frame loop, so only main is
    // reserved: on a 3-core CI runner that is the difference between one worker and two
    const unsigned reserved_threads = app_config_.render_thread && !app_config_.cook_mode ? 2u : 1u;
    task_manager_ = std::make_unique<TaskManager>(app_config_.max_threads, reserved_threads);
    TaskDispatcher::Instance().RegisterTaskQueue(pending_tasks_, ThreadName::Main);

#if ENABLE_PROFILER
    Profiler::RegisterThreadForProfiling("Main");
#endif

#ifdef __has_feature
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
        // a valid view at this point means the UI app attached its window; running that
        // windowed loop with a headless config would starve the drawable queue on ios
        if (app_config_.platform == AppConfig::NativePlatform::IOS && view_->IsValid())
        {
            Log(Error, "Headless mode on iOS only works for processes launched with --headless, not the UI app.");
            return false;
        }
#if !FRAMEWORK_GLFW && !FRAMEWORK_MACOS && !FRAMEWORK_IOS && !FRAMEWORK_ANDROID
        Log(Error, "Headless mode is currently supported only on GLFW, macOS, iOS and Android frameworks.");
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

        input_manager_ = std::make_unique<InputManager>(app_config_, ui_manager_.get());
        SetupInputHandlers();
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

    Log(Info, "Default scene loading task dispatched");

    frame_timer_.Reset();

    CoreStates::Instance().SetAppState(CoreStates::AppState::MainLoop);

    initialized_ = true;

    Log(Info, "Init success. Main loop started");

    return true;
}

int AppFramework::RunCookMode()
{
    ASSERT_F(core_initialized_, "Core is not initialized. Call InitCore first");

    // an RHI is only an accelerator here: no render framework, and the main thread acts as
    // the render thread so GPU work runs inline
    if (view_)
    {
        view_->InitGUI(this);

        rhi_ = RHIContext::CreateRHI(rhi_config_);
        std::string rhi_error;
        if (rhi_ && rhi_->InitRHI(view_, rhi_error))
        {
            rhi_->InitRenderResources();
            render_config_.SetupBackend(rhi_.get(), view_);
            ThreadManager::RegisterRenderThread();
            Log(Info, "cook mode with RHI. physical gpu: {}", rhi_->HasPhysicalGpu());
        }
        else
        {
            Log(Info, "cook mode without RHI. {}", rhi_error);
            rhi_ = nullptr;
        }
    }

    const auto cook_targets = CookTargets::Parse(app_config_.cook_targets);
    if (cook_targets.empty())
    {
        return 1;
    }

    auto exit_code = RunCookPipeline(cook_targets, app_config_.scene, rhi_.get(), render_config_);

    if (rhi_)
    {
        rhi_->WaitForDeviceIdle();
        rhi_->Cleanup();
        rhi_ = nullptr;
    }

    // Cleanup() is for fully-initialized apps; tear down the core-only state here
    task_manager_ = nullptr;
    FileManager::DestroyNativeFileManager();

    Log(Info, "App exit gracefully.");
    return exit_code;
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

        view_->Tick();

        if (view_->ShouldClose())
        {
            return true;
        }

        input_manager_->DispatchPendingEvents();
    }

    if (scene_load_task_ && scene_load_task_->IsReady())
    {
        const bool scene_loaded = scene_load_task_->Get();
        scene_load_task_.reset();
        scene_file_loaded_ = true;
        if (scene_loaded)
        {
            Log(Info, "Scene file loaded");

            // the loaded scene may bring its own main camera, so this must wait until now
            session_manager_->ApplyCamera(GetMainCamera());
        }
        else
        {
            Log(Error, "Scene file failed to load");
        }
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
            Log(Info, "Test case '{}' passed", test_case_->GetName());
            RequestExit();
        }
        else if (result == TestCase::Result::Fail)
        {
            Log(Error, "Test case '{}' failed", test_case_->GetName());
            exit_code_ = 1;
            RequestExit();
        }
    }
#endif

    {
        PROFILE_SCOPE("MainLoop tick scene");

        main_scene_->Tick();
        main_scene_->ProcessChange();
    }

    {
        PROFILE_SCOPE("MainLoop render ui");

        if (ui_manager_)
        {
            if (view_->CanRender())
            {
                DrawUi();
            }

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

        input_subscriptions_.clear();
        input_manager_ = nullptr;

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

void AppFramework::PushInputEvent(const InputEvent &event)
{
    if (input_manager_)
    {
        input_manager_->Push(event);
    }
}

void AppFramework::SetupInputHandlers()
{
    input_subscriptions_.push_back(input_manager_->OnSceneDoubleTap().Subscribe(
        [this](uint8_t /*finger_count*/) { show_control_panel_ = !show_control_panel_; }));

    input_subscriptions_.push_back(input_manager_->OnSceneTap().Subscribe([this](uint8_t finger_count) {
        if (finger_count == 4)
        {
            CaptureNextFrames(1);
        }
    }));

    // escape has two owners: the panel dismisses itself first, and only a key it leaves alone
    // reaches the app and exits
    input_subscriptions_.push_back(input_manager_->BindKey(InputLayer::Ui, {.key = Key::Escape}, [this]() {
        if (!show_control_panel_)
        {
            return false;
        }

        show_control_panel_ = false;
        return true;
    }));

    input_subscriptions_.push_back(input_manager_->BindKey(InputLayer::Scene, {.key = Key::Escape}, []() {
        RequestExit();
        return true;
    }));

    // the app owns the config instance the per-frame snapshot is taken from, so it holds the
    // subscriptions the renderer binds against it
    auto render_config_subscriptions = render_config_.BindInput();
    std::ranges::move(render_config_subscriptions, std::back_inserter(input_subscriptions_));
}

void AppFramework::ResetInputEvents()
{
    if (input_manager_)
    {
        input_manager_->Reset();
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
