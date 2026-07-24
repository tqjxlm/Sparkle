#include "application/AppFramework.h"

#include "application/InputManager.h"
#include "application/NativeKeyboard.h"
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
#include "core/cook/CookArtifactStore.h"
#include "core/task/TaskManager.h"
#include "io/TextureCookJob.h"
#include "renderer/denoiser/DenoiserConfig.h"
#include "renderer/nrd/NrdConfig.h"
#include "renderer/resource/IblCookAccelerator.h"
#include "renderer/resource/IblCookPlan.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/light/SkyLight.h"
#include "scene/component/primitive/PrimitiveComponent.h"
#include "scene/cook/SceneCooker.h"
#include "scene/material/Material.h"
#include "scene/material/MaterialManager.h"

#if ENABLE_TEST_CASES
#include "application/TestCase.h"
#endif

#include <IconsFontAwesome7.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <unordered_set>

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

    task_manager_ = std::make_unique<TaskManager>(app_config_.max_threads);
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

static const std::map<std::string, TextureCompression::Family> &CookTargetFamilies()
{
    static const std::map<std::string, TextureCompression::Family> Families{
        {"android", TextureCompression::Family::Astc},    {"ios", TextureCompression::Family::Astc},
        {"macos", TextureCompression::Family::Astc},      {"macos-glfw", TextureCompression::Family::Astc},
        {"windows-glfw", TextureCompression::Family::Bc}, {"linux-glfw", TextureCompression::Family::Bc},
    };
    return Families;
}

static const char *SelfCookTarget()
{
#if FRAMEWORK_GLFW
#if PLATFORM_MACOS
    return "macos-glfw";
#elif PLATFORM_WINDOWS
    return "windows-glfw";
#else
    return "linux-glfw";
#endif
#elif PLATFORM_MACOS
    return "macos";
#elif PLATFORM_IOS
    return "ios";
#else
    return "android";
#endif
}

static std::vector<std::string> ParseCookTargets(const std::string &config)
{
    const std::string requested = config.empty() ? SelfCookTarget() : config;

    std::vector<std::string> targets;
    for (size_t begin = 0; begin <= requested.size();)
    {
        const auto end = std::min(requested.find('+', begin), requested.size());
        auto target = requested.substr(begin, end - begin);
        begin = end + 1;

        if (target.empty())
        {
            continue;
        }
        if (!CookTargetFamilies().contains(target))
        {
            Log(Error,
                "unknown cook target '{}'. known targets: android, ios, macos, macos-glfw, windows-glfw, "
                "linux-glfw",
                target);
            return {};
        }
        if (std::ranges::find(targets, target) == targets.end())
        {
            targets.push_back(std::move(target));
        }
    }

    if (targets.empty())
    {
        Log(Error, "no cook target requested");
    }
    return targets;
}

static std::string JobManifestKey(const CookJob &job)
{
    return CookArtifactStore::GetManifestKey({.type = job.GetType(),
                                              .version = job.GetVersion(),
                                              .source_name = job.GetSourceName(),
                                              .source_hash = std::nullopt});
}

using ConsumedSourceMap = std::map<std::string, std::map<TextureCompression::Family, std::set<std::string>>>;

static void CollectMaterialTextureJobs(const Scene &scene, const std::set<TextureCompression::Family> &families,
                                       std::vector<std::unique_ptr<CookJob>> &jobs, ConsumedSourceMap &consumed_sources)
{
    std::unordered_set<std::string> seen;
    for (const auto *primitive : scene.GetPrimitives())
    {
        const auto *material = primitive->GetMaterial();
        if (material == nullptr)
        {
            continue;
        }

        ForEachMaterialTexture(material->GetRawMaterial(), [&seen, &jobs, &families,
                                                            &consumed_sources](const std::shared_ptr<Image2D> &texture,
                                                                               TextureCompression::Profile profile) {
            if (!texture || !IsCookableMaterialTexture(*texture))
            {
                return;
            }

            for (auto family : families)
            {
                auto job = std::make_unique<TextureCookJob>(texture, texture->GetName(), profile, family);
                auto manifest_key = JobManifestKey(*job);
                consumed_sources[texture->GetName()][family].insert(manifest_key);
                if (seen.insert(std::move(manifest_key)).second)
                {
                    jobs.push_back(std::move(job));
                }
            }
        });
    }
}

// read by assemble_cooked_image in build.py; the cook output dir persists across runs,
// so a smaller result must still overwrite a stale file
static bool WriteCookProducts(const std::vector<std::string> &targets, const std::set<std::string> &universal_keys,
                              const ConsumedSourceMap &consumed_sources)
{
    nlohmann::json json = nlohmann::json::object();
    for (const auto &target : targets)
    {
        const auto family = CookTargetFamilies().at(target);

        std::set<std::string> artifacts = universal_keys;
        nlohmann::json consumed_json = nlohmann::json::object();
        for (const auto &[source, family_keys] : consumed_sources)
        {
            const auto keys = family_keys.find(family);
            if (keys == family_keys.end())
            {
                continue;
            }
            artifacts.insert(keys->second.begin(), keys->second.end());
            consumed_json[source] = keys->second;
        }

        json[target] = {{"artifacts", artifacts}, {"consumed_sources", consumed_json}};
    }

    const auto dump = json.dump(2);
    const auto written = FileManager::GetNativeFileManager()->Write(Path::Internal("cooked/cook_products.json"),
                                                                    dump.data(), dump.size());
    return !written.empty();
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

    SceneCooker::JobAccelerator accelerator;
    if (rhi_ && rhi_->HasPhysicalGpu())
    {
        accelerator = [this](const CookJob &job) {
            return IblCookAccelerator::TryCook(job, rhi_.get(), render_config_);
        };
    }

    const auto cook_targets = ParseCookTargets(app_config_.cook_targets);
    if (cook_targets.empty())
    {
        return 1;
    }

    std::set<TextureCompression::Family> texture_families;
    for (const auto &target : cook_targets)
    {
        texture_families.insert(CookTargetFamilies().at(target));
        Log(Info, "cook target: {}", target);
    }

    // scene loading must keep raw material textures so the plan can cook the requested families
    SetMaterialTextureInlineResolve(false);

    ConsumedSourceMap consumed_texture_sources;
    std::set<std::string> universal_keys;

    const SceneCooker::JobPlan job_plan{.collect_scene_independent_jobs =
                                            [&universal_keys](std::vector<std::unique_ptr<CookJob>> &jobs) {
                                                IblCookPlan::CollectSceneIndependentJobs(jobs);
                                                for (const auto &job : jobs)
                                                {
                                                    universal_keys.insert(JobManifestKey(*job));
                                                }
                                                return true;
                                            },
                                        .collect_scene_jobs =
                                            [&consumed_texture_sources, &texture_families, &universal_keys](
                                                const Scene &scene, std::vector<std::unique_ptr<CookJob>> &jobs) {
                                                const auto *sky_light = scene.GetSkyLight();
                                                CollectMaterialTextureJobs(scene, texture_families, jobs,
                                                                           consumed_texture_sources);
                                                const auto family_scoped_jobs = jobs.size();

                                                if (sky_light != nullptr)
                                                {
                                                    const auto &environment = sky_light->GetCubeMap();
                                                    if (!environment)
                                                    {
                                                        return !sky_light->HasSkyMap();
                                                    }
                                                    // no job produces the sky cube: it cooked during scene load
                                                    universal_keys.insert(sky_light->GetCookManifestKey());
                                                    IblCookPlan::CollectEnvironmentJobs(environment, jobs);
                                                }

                                                for (auto i = family_scoped_jobs; i < jobs.size(); i++)
                                                {
                                                    universal_keys.insert(JobManifestKey(*jobs[i]));
                                                }
                                                return true;
                                            }};

    auto exit_code = SceneCooker::Run(app_config_.scene, job_plan, accelerator);

    if (exit_code == 0 && !WriteCookProducts(cook_targets, universal_keys, consumed_texture_sources))
    {
        Log(Error, "failed to write the cook products manifest");
        exit_code = 1;
    }

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

void AppFramework::DebugNextFrame()
{
    auto scale = view_->GetWindowScale();
    auto pointer = input_manager_->GetPointerPosition();
    TaskManager::RunInRenderThread([this, scale, pointer]() {
        render_framework_->SetDebugPoint(pointer.x() * scale.x(),
                                         static_cast<float>(render_config_.image_height) - pointer.y() * scale.y());
    });
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
    input_subscriptions_.push_back(input_manager_->OnScenePointer().Subscribe([this](const PointerEvent &event) {
        switch (event.action)
        {
        case PointerAction::Down:
            if (event.button == ClickButton::SecondaryRight ||
                (event.modifiers & static_cast<uint32_t>(KeyboardModifier::Control)))
            {
                DebugNextFrame();
            }
            else if (event.button == ClickButton::PrimaryLeft)
            {
                if (auto *camera = GetMainCamera())
                {
                    camera->OnPointerDown();
                }
            }
            break;
        case PointerAction::Up:
        case PointerAction::Cancel:
            if (event.button == ClickButton::PrimaryLeft)
            {
                if (auto *camera = GetMainCamera())
                {
                    camera->OnPointerUp();
                }
            }
            break;
        default:
            break;
        }
    }));

    input_subscriptions_.push_back(input_manager_->OnSceneDrag().Subscribe([this](Vector2 delta) {
        if (auto *camera = GetMainCamera())
        {
            camera->OnPointerMove(delta.y(), -delta.x());
        }
    }));

    input_subscriptions_.push_back(input_manager_->OnSceneZoom().Subscribe([this](float amount) {
        if (auto *camera = GetMainCamera())
        {
            camera->OnScroll(amount);
        }
    }));

    input_subscriptions_.push_back(input_manager_->OnSceneDoubleTap().Subscribe(
        [this](uint8_t /*finger_count*/) { show_control_panel_ = !show_control_panel_; }));

    input_subscriptions_.push_back(input_manager_->OnSceneTap().Subscribe([this](uint8_t finger_count) {
        if (finger_count == 4)
        {
            CaptureNextFrames(1);
        }
    }));

    input_subscriptions_.push_back(
        input_manager_->OnSceneKey().Subscribe([this](const KeyEvent &event) { HandleSceneKey(event); }));
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

void AppFramework::HandleSceneKey(const KeyEvent &event)
{
    auto *camera = GetMainCamera();
    if (!camera)
    {
        return;
    }

    const auto action = event.action;
    const bool shift_on = (event.modifiers & static_cast<uint32_t>(KeyboardModifier::Shift)) != 0;

    int key = event.key;
#if FRAMEWORK_MACOS
    // support keyboards with no escape key
    if (static_cast<NativeKeyboard>(key) == NativeKeyboard::KeyDelete)
    {
        key = static_cast<int>(NativeKeyboard::KeyEscape);
    }
#endif

    switch (static_cast<NativeKeyboard>(key))
    {
    case NativeKeyboard::KeyEscape: {
        if (action == KeyAction::Release)
        {
            RequestExit();
        }
        break;
    }
    case NativeKeyboard::KeySpace: {
        render_config_.accumulate_key_held = (action == KeyAction::Press);
        break;
    }
    case NativeKeyboard::KeyUp: {
        if (action == KeyAction::Release)
        {
            camera->SetAperture(camera->GetAttribute().aperture + 1.f);
        }
        break;
    }
    case NativeKeyboard::KeyDown: {
        if (action == KeyAction::Release)
        {
            camera->SetAperture(camera->GetAttribute().aperture - 1.f);
        }
        break;
    }
    case NativeKeyboard::KeyP: {
        if (action == KeyAction::Release)
        {
            camera->PrintPosture();
        }
        break;
    }
    case NativeKeyboard::KeyKpAdd: {
        if (action == KeyAction::Release)
        {
            Log(Debug, "Add debug sphere");
            SceneManager::GenerateRandomSpheres(*main_scene_, 1);
        }
        break;
    }
    case NativeKeyboard::KeyEqual: {
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
    case NativeKeyboard::KeyMinus: {
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
