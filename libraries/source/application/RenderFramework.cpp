#include "application/RenderFramework.h"

#include "application/InputManager.h"
#include "application/NativeView.h"
#include "application/UiManager.h"
#include "core/ConfigManager.h"
#include "core/CoreStates.h"
#include "core/FileManager.h"
#include "core/Path.h"
#include "core/Profiler.h"
#include "core/ThreadManager.h"
#include "core/task/TaskDispatcher.h"
#include "core/task/TaskManager.h"
#include "renderer/RenderConfig.h"
#include "renderer/graph/RGTexturePool.h"
#include "renderer/pass/PostChain.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/renderer/Renderer.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "scene/SceneNode.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <utility>

constexpr float LogInterval = 1.f;

namespace sparkle
{
namespace
{
std::string SanitizeFileNameToken(std::string_view value, size_t max_length = 64)
{
    std::string sanitized(value.substr(0, max_length));
    for (auto &ch : sanitized)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '_' && ch != '-')
        {
            ch = '_';
        }
    }

    if (sanitized.empty())
    {
        sanitized = "scene";
    }

    return sanitized;
}

std::string BuildScreenshotName(const Scene *scene, RenderConfig::Pipeline pipeline)
{
    ASSERT(scene && scene->GetRootNode());

    auto scene_name = SanitizeFileNameToken(scene->GetRootNode()->GetName());

    const auto now_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    return std::format("{}_{}_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}", scene_name, Enum2Str(pipeline),
                       local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday, local_tm.tm_hour,
                       local_tm.tm_min, local_tm.tm_sec);
}

// writes `dump` to screenshots/<name>.json
void SaveGraphDump(const nlohmann::json &dump, const std::string &name)
{
    const auto path = (std::filesystem::path("screenshots") / (name + ".json")).string();
    const auto text = dump.dump(4);
    const auto saved_path = FileManager::GetNativeFileManager()->Write(Path::External(path), text.data(), text.size());
    if (saved_path.empty())
    {
        Log(Error, "Failed to save render graph dump to {}", path);
    }
    else
    {
        Log(Info, "Render graph dump saved to {}", path);
    }
}

// a string field of an entry of a render graph dump
const char *DumpString(const nlohmann::json &entry, const char *key)
{
    return entry.at(key).get_ref<const std::string &>().c_str();
}

// the texture the dumped graph shows in place of the frame, null when it shows the frame
const char *FindViewedTexture(const nlohmann::json &dump)
{
    for (const auto &pass : dump.at("passes"))
    {
        if (pass.at("name").get_ref<const std::string &>() == PostChain::GraphViewPassName)
        {
            return DumpString(pass.at("accesses").at(0), "resource");
        }
    }
    return nullptr;
}

// the name of the pass a dumped resource names by index under `key`, e.g. its first use
const char *DumpPassName(const nlohmann::json &dump, const nlohmann::json &resource, const char *key)
{
    return DumpString(dump.at("passes").at(resource.at(key).get<size_t>()), "name");
}

// the format and size class of a dumped transient, e.g. "RGBAFloat16 Scene", or the kind of an import
std::string DescribeResource(const nlohmann::json &resource)
{
    if (!resource.contains("format"))
    {
        return resource.at("kind").get<std::string>();
    }

    auto description = std::format("{} {}", DumpString(resource, "format"), DumpString(resource, "size_class"));
    if (resource.contains("width"))
    {
        description +=
            std::format(" {}x{}", resource.at("width").get<uint32_t>(), resource.at("height").get<uint32_t>());
    }
    return description;
}
} // namespace

RenderFramework::RenderFramework(NativeView *native_view, RHIContext *rhi, UiManager *ui_manager, Scene *scene)
    : native_view_(native_view), rhi_(rhi), ui_manager_(ui_manager), scene_(scene),
      frame_rate_monitor_(LogInterval, false, [this](float delta_time) { MeasurePerformance(delta_time); })
{
    graph_texture_pool_ = std::make_unique<RGTexturePool>(rhi_);
    task_queue_ = std::make_shared<ThreadTaskQueue>();
    TaskDispatcher::Instance().RegisterTaskQueue(task_queue_, ThreadName::Render);

    if (auto *input_manager = InputManager::Instance())
    {
        secondary_click_subscription_ =
            input_manager->OnSceneSecondaryClick().Subscribe([this](Vector2 position) { RequestDebugPoint(position); });
    }
}

RenderFramework::~RenderFramework() = default;

void RenderFramework::RequestDebugPoint(const Vector2 &ui_position)
{
    const auto scale = native_view_->GetWindowScale();

    TaskManager::RunInRenderThread([this, ui_position, scale]() {
        // render_config_ belongs to the render thread, so the y flip reads the resolution here
        SetDebugPoint(ui_position.x() * scale.x(),
                      static_cast<float>(render_config_.image_height) - ui_position.y() * scale.y());
    });
}

void RenderFramework::RenderThreadMain()
{
    ThreadManager::RegisterRenderThread();

    {
        std::unique_lock<std::mutex> lock(task_queue_mutex_);
        render_loop_started_ = true;
        render_thread_started_.notify_all();

        Log(Info, "Render thread started.");
    }

    while (!should_stop_)
    {
        RenderLoop();
        end_of_frame_signal_.notify_all();
    }

    // discard all remaining tasks as we are going to exit
    {
        std::unique_lock<std::mutex> lock(task_queue_mutex_);
        while (!tasks_per_frame_.empty())
        {
            tasks_per_frame_.pop();
        }
    }
    end_of_frame_signal_.notify_all();

    Log(Info, "Render thread about to exit.");

    rhi_->WaitForDeviceIdle();

    renderer_ = nullptr;
    graph_texture_pool_ = nullptr;

    Log(Info, "Render thread exit.");
}

void RenderFramework::RenderLoop()
{
#if FRAMEWORK_APPLE
    @autoreleasepool
    {
#endif
        static const char *thread_name = "render thread";

        PROFILE_FRAME_START(thread_name);

        ASSERT(ThreadManager::IsInRenderThread());

        Timer render_thread_timer;

        if (BeginFrame())
        {
            renderer_->Tick();

            if (const auto dump = renderer_->Render(!graph_dump_consumers_.empty()))
            {
                for (const auto &on_dump : std::exchange(graph_dump_consumers_, {}))
                {
                    on_dump(dump);
                }
            }

            ready_for_auto_screenshot_.store(IsSceneFullyLoaded() && renderer_->IsReadyForAutoScreenshot(),
                                             std::memory_order_release);

            ProcessScreenshotRequest();

            EndFrame();
        }

        AdvanceFrame(static_cast<float>(render_thread_timer.ElapsedMicroSecond()) * 1e-3f);

        PROFILE_FRAME_END(thread_name);

#if FRAMEWORK_APPLE
    }
#endif
}

void RenderFramework::AdvanceFrame(float render_thread_time)
{
    last_second_render_thread_time_ += render_thread_time;

    float gpu_time = rhi_->GetFrameStats(rhi_->GetFrameIndex()).elapsed_time_ms;
    if (gpu_time > 0.f)
    {
        last_second_gpu_time_ += gpu_time;
    }

    frame_rate_monitor_.Tick();
}

void RenderFramework::MeasurePerformance([[maybe_unused]] float delta_time)
{
    static uint64_t last_frame_number = 0;
    const auto last_second_frame_cnt = static_cast<float>(frame_number_ - last_frame_number);
    last_frame_number = frame_number_;

    Logger::LogToScreen("RenderThread", std::format("Render thread: {:.1f} ms",
                                                    last_second_render_thread_time_ / last_second_frame_cnt));
    Logger::LogToScreen("GPU", std::format("GPU: {:.1f} ms", last_second_gpu_time_ / last_second_frame_cnt));

    last_second_render_thread_time_ = 0;
    last_second_gpu_time_ = 0;
}

void RenderFramework::PushRenderTasks()
{
    ASSERT(ThreadManager::IsInMainThread());

    std::unique_lock<std::mutex> lock(task_queue_mutex_);

    {
        PROFILE_SCOPE("MainLoop wait for render thread");
        can_push_new_tasks_.wait(lock, [this]() { return tasks_per_frame_.size() < MaxBufferedTaskFrames; });
    }

    tasks_per_frame_.push(task_queue_->PopTasks());

    new_task_pushed_.notify_all();
}

void RenderFramework::SetDebugPoint(float x, float y)
{
    ASSERT(ThreadManager::IsInRenderThread());

    renderer_->SetDebugPoint(x, y);
}

void RenderFramework::OnFrameBufferResize(int width, int height)
{
    ASSERT(ThreadManager::IsInRenderThread());

    if (!renderer_)
    {
        return;
    }

    Log(Info, "Frame buffer resize [{}, {}]", width, height);
    renderer_->OnFrameBufferResize(width, height);
}

void RenderFramework::NewFrame(uint64_t frame_number, const RenderConfig &render_config)
{
    ASSERT(ThreadManager::IsInRenderThread());

    frame_number_ = frame_number;
    render_config_ = render_config;
}

void RenderFramework::StartRenderThread(const RenderConfig &render_config)
{
    render_thread_ = std::thread(&RenderFramework::RenderThreadMain, this);
    render_config_ = render_config;

    std::unique_lock<std::mutex> lock(thread_mutex_);
    render_thread_started_.wait(lock, [this]() { return render_loop_started_; });
}

void RenderFramework::WaitUntilIdle()
{
    std::unique_lock<std::mutex> lock(task_queue_mutex_);

    end_of_frame_signal_.wait(lock, [this]() { return tasks_per_frame_.empty(); });
}

void RenderFramework::StopRenderThread()
{
    Log(Info, "Wait for render thread to stop");

    should_stop_ = true;

    // in case the render thread is waiting for new tasks
    new_task_pushed_.notify_all();

    WaitUntilIdle();

    render_thread_.join();

    ThreadManager::UnregisterRenderThread();
}

void RenderFramework::RecreateRendererIfNecessary()
{
    bool should_recreate = !renderer_ || render_config_.pipeline != renderer_->GetRenderMode() ||
                           render_config_.GetResolution() != renderer_->GetResolution();
    if (!should_recreate)
    {
        return;
    }

    // the camera proxy arrives via a queued render-thread task after scene setup; a renderer
    // created before that would initialize against a camera-less scene proxy
    if (!scene_->GetRenderProxy()->GetCamera())
    {
        return;
    }

    PROFILE_SCOPE_LOG("RecreateRenderer");

    Log(Info, "Recreating renderer, render mode: {}", Enum2Str(render_config_.pipeline));

    rhi_->WaitForDeviceIdle();

    if (renderer_)
    {
        // recreate all render proxies. this can be expensive.
        scene_->RecreateRenderProxy();

        renderer_ = nullptr;

        // after WaitForDeviceIdle, it is safe to delete all deferred deletions.
        rhi_->FlushDeferredDeletions();
    }

    renderer_ = Renderer::CreateRenderer(render_config_, rhi_, scene_->GetRenderProxy(), *graph_texture_pool_);

    // the scene-loaded notification is one-shot; a renderer created after it must not miss it
    // (readiness stays false otherwise, and e.g. GPURenderer then resets NRD history every frame)
    if (scene_loaded_notified_)
    {
        renderer_->NotifySceneLoaded();
    }

    renderer_created_event_.Trigger();
}

bool RenderFramework::BeginFrame()
{
    PROFILE_SCOPE("RenderFramework::BeginFrame");

    ConsumeRenderThreadTasks();

    if (should_stop_)
    {
        return false;
    }

    if (!native_view_->CanRender())
    {
        return false;
    }

    if (ui_manager_)
    {
        ui_manager_->BeginRenderThread();
        render_config_.render_ui = UiManager::HasDataToDraw();
    }
    else
    {
        render_config_.render_ui = false;
    }

    RecreateRendererIfNecessary();

    if (!renderer_ || !native_view_->CanRender())
    {
        return false;
    }

    if (!rhi_->BeginFrame())
    {
        return false;
    }

    return true;
}

void RenderFramework::EndFrame()
{
    PROFILE_SCOPE("RenderFramework::EndFrame");

    // a recorded frame is submitted even when the surface is lost while it records: recording already advanced the
    // CPU-side state (tracked resource states, staged builds, timer queries), and a lost surface only fails the present
    rhi_->EndFrame();

    // reset debug point
    renderer_->SetDebugPoint(-1., -1.);

    if (!native_view_->CanRender())
    {
        Log(Debug, "lost rendering surface. releasing render resources now...");

        rhi_->ReleaseRenderResources();
        rhi_->DestroySurface();
    }
}

void RenderFramework::ConsumeRenderThreadTasks()
{
    std::vector<std::function<void()>> frame_tasks;

    // 1. pop next frame's tasks
    {
        std::unique_lock<std::mutex> lock(task_queue_mutex_);

        if (tasks_per_frame_.empty())
        {
            PROFILE_SCOPE("render thread starving");

            new_task_pushed_.wait(lock);
        }

        if (tasks_per_frame_.empty())
        {
            return;
        }

        std::swap(frame_tasks, tasks_per_frame_.front());

        tasks_per_frame_.pop();
    }

    // 2. tell the main thread that new tasks are welcome
    can_push_new_tasks_.notify_all();

    // 3. run tasks
    for (auto &task : frame_tasks)
    {
        task();
    }
}

void RenderFramework::NotifySceneLoaded()
{
    TaskManager::RunInRenderThread([this]() {
        if (renderer_)
        {
            renderer_->NotifySceneLoaded();
        }
        scene_loaded_notified_ = true;
    });
}

bool RenderFramework::IsSceneFullyLoaded() const
{
    return scene_loaded_notified_;
}

const RGTexturePool &RenderFramework::GetGraphTexturePool() const
{
    return *graph_texture_pool_;
}

std::shared_ptr<ScreenshotRequest> RenderFramework::RequestTakeScreenshot(const std::string &name)
{
    auto request = std::make_shared<ScreenshotRequest>(name);
    {
        std::scoped_lock<std::mutex> lock(screenshot_queue_mutex_);
        screenshot_queue_.push(request);
    }
    return request;
}

std::shared_ptr<ScreenshotRequest> RenderFramework::RequestGraphDump(const std::string &name)
{
    auto request = std::make_shared<ScreenshotRequest>(name);
    TaskManager::RunInRenderThread([this, request] {
        graph_dump_consumers_.emplace_back([request](const std::shared_ptr<const nlohmann::json> &dump) {
            SaveGraphDump(*dump, request->GetName());
            request->MarkCompleted();
        });
    });
    return request;
}

bool RenderFramework::IsReadyForAutoScreenshot() const
{
    return ready_for_auto_screenshot_.load(std::memory_order_acquire);
}

void RenderFramework::ProcessScreenshotRequest()
{
    if (active_screenshot_ || !renderer_ || !IsSceneFullyLoaded())
    {
        return;
    }

    {
        std::scoped_lock<std::mutex> lock(screenshot_queue_mutex_);
        if (screenshot_queue_.empty())
        {
            return;
        }
        active_screenshot_ = std::move(screenshot_queue_.front());
        screenshot_queue_.pop();
    }

    Log(Info, "Screenshot requested: {}", active_screenshot_->GetName());
    renderer_->RequestSaveScreenshot(active_screenshot_->GetName(), false, [this]() {
        active_screenshot_->MarkCompleted();
        active_screenshot_.reset();
    });
}

void RenderFramework::DrawUi()
{
    ImGui::TextUnformatted("Screenshot");
    ImGui::Separator();

    const bool saving = screenshot_saving_.load();

    ImGui::BeginDisabled(saving);
    if (ImGui::Button("Save Screenshot"))
    {
        auto screenshot_name = BuildScreenshotName(scene_, render_config_.pipeline) + ".png";
        const bool capture_ui = should_capture_ui_;

        last_saved_screenshot_path_ = screenshot_name;

        screenshot_saving_.store(true);

        TaskManager::RunInRenderThread([this, screenshot_name, capture_ui]() {
            renderer_->RequestSaveScreenshot(screenshot_name, capture_ui,
                                             [this]() { screenshot_saving_.store(false); });
        });
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    ImGui::Checkbox("With UI", &should_capture_ui_);

    ImGui::PopStyleVar();

    if (!last_saved_screenshot_path_.empty())
    {
        ImGui::TextWrapped(saving ? "Saving: %s" : "Saved: %s", last_saved_screenshot_path_.c_str());
    }
}

void RenderFramework::DrawGraphUi()
{
    TaskManager::RunInRenderThread([this]() {
        graph_dump_consumers_.emplace_back([this](std::shared_ptr<const nlohmann::json> dump) {
            std::scoped_lock<std::mutex> lock(graph_dump_mutex_);
            graph_dump_ = std::move(dump);
        });
    });

    ImGui::TextUnformatted("Render Graph");
    ImGui::Separator();

    if (ImGui::Button("Save Graph Dump"))
    {
        saved_graph_dump_ = RequestGraphDump(BuildScreenshotName(scene_, render_config_.pipeline));
    }

    if (saved_graph_dump_)
    {
        ImGui::TextWrapped(saved_graph_dump_->IsCompleted() ? "Saved: %s.json" : "Saving: %s.json",
                           saved_graph_dump_->GetName().c_str());
    }

    std::shared_ptr<const nlohmann::json> graph_dump;
    {
        std::scoped_lock<std::mutex> lock(graph_dump_mutex_);
        graph_dump = graph_dump_;
    }

    if (!graph_dump)
    {
        return;
    }

    const auto *viewed_texture = FindViewedTexture(*graph_dump);
    ImGui::Text("Shows: %s", viewed_texture ? viewed_texture : "the frame");

    ImGui::SeparatorText("Passes");

    if (ImGui::BeginTable("passes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto &pass : graph_dump->at("passes"))
        {
            const bool culled = pass.at("culled").get<bool>();

            ImGui::TableNextRow();
            ImGui::BeginDisabled(culled);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(DumpString(pass, "name"));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(DumpString(pass, "kind"));

            ImGui::TableNextColumn();
            if (culled)
            {
                ImGui::TextWrapped("culled: %s", DumpString(pass, "cull_reason"));
            }
            else if (pass.contains("gpu_ms"))
            {
                ImGui::Text("%.3f", pass.at("gpu_ms").get<double>());
            }

            ImGui::EndDisabled();
        }

        ImGui::EndTable();
    }

    ImGui::SeparatorText("Resources");

    auto *view_config = ConfigManager::Instance().GetConfig<std::string>("render_graph_view");
    ASSERT(view_config);
    const auto view = view_config->Get();

    // selecting a texture shows it in place of the frame
    if (ImGui::BeginTable("resources", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Resource");
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Uses", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable("Frame", view.empty(), ImGuiSelectableFlags_SpanAllColumns) && !view.empty())
        {
            view_config->Set("");
        }

        for (const auto &resource : graph_dump->at("resources"))
        {
            const auto &name = resource.at("name").get_ref<const std::string &>();

            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            if (resource.at("type").get_ref<const std::string &>() != "Texture")
            {
                ImGui::TextUnformatted(name.c_str());
            }
            else if (ImGui::Selectable(name.c_str(), name == view, ImGuiSelectableFlags_SpanAllColumns) && name != view)
            {
                view_config->Set(name);
            }

            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", DescribeResource(resource).c_str());

            ImGui::TableNextColumn();
            if (resource.contains("first_use"))
            {
                ImGui::TextWrapped("%s..%s", DumpPassName(*graph_dump, resource, "first_use"),
                                   DumpPassName(*graph_dump, resource, "last_use"));
            }
        }

        ImGui::EndTable();
    }
}
} // namespace sparkle
