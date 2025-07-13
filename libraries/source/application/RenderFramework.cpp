#include "application/RenderFramework.h"

#include "application/NativeView.h"
#include "application/UiManager.h"
#include "core/Profiler.h"
#include "core/TaskManager.h"
#include "core/ThreadManager.h"
#include "renderer/RenderConfig.h"
#include "renderer/renderer/Renderer.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"

#include <mutex>

constexpr float LogInterval = 1.f;

namespace sparkle
{
RenderFramework::RenderFramework(NativeView *native_view, RHIContext *rhi, UiManager *ui_manager, Scene *scene)
    : native_view_(native_view), rhi_(rhi), ui_manager_(ui_manager), scene_(scene),
      frame_rate_monitor_(LogInterval, false, [this](float delta_time) { MeasurePerformance(delta_time); })
{
}

RenderFramework::~RenderFramework() = default;

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
    while (!frame_task_queue_.empty())
    {
        frame_task_queue_.pop();
    }
    end_of_frame_signal_.notify_all();

    Log(Debug, "Render thread about to exit.");

    rhi_->WaitForDeviceIdle();

    renderer_ = nullptr;

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

            renderer_->Render();

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
        can_push_new_tasks_.wait(lock, [this]() { return frame_task_queue_.size() < RenderTaskBufferSize; });
    }

    frame_task_queue_.push(TaskManager::Instance().PopRenderThreadTasks());

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

void RenderFramework::StartRenderThread()
{
    render_thread_ = std::thread(&RenderFramework::RenderThreadMain, this);

    std::unique_lock<std::mutex> lock(thread_mutex_);
    render_thread_started_.wait(lock, [this]() { return render_loop_started_; });
}

void RenderFramework::WaitUntilIdle()
{
    std::unique_lock<std::mutex> lock(task_queue_mutex_);

    end_of_frame_signal_.wait(lock, [this]() { return frame_task_queue_.empty(); });
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
    bool should_recreate = !renderer_ || render_config_.pipeline != renderer_->GetRenderMode();
    if (!should_recreate)
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

    renderer_ = Renderer::CreateRenderer(render_config_, rhi_, scene_->GetRenderProxy());

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

    ui_manager_->BeginRenderThread();

    render_config_.render_ui = UiManager::HasDataToDraw();

    RecreateRendererIfNecessary();

    rhi_->BeginFrame();

    return true;
}

void RenderFramework::EndFrame()
{
    PROFILE_SCOPE("RenderFramework::EndFrame");

    if (!native_view_->CanRender())
    {
        Log(Debug, "lost rendering surface. releasing render resources now...");

        rhi_->ReleaseRenderResources();
        rhi_->DestroySurface();

        return;
    }

    rhi_->EndFrame();

    // reset debug point
    renderer_->SetDebugPoint(-1., -1.);
}

void RenderFramework::ConsumeRenderThreadTasks()
{
    std::vector<std::function<void()>> frame_tasks;

    // 1. pop next frame's tasks
    {
        std::unique_lock<std::mutex> lock(task_queue_mutex_);

        if (frame_task_queue_.empty())
        {
            PROFILE_SCOPE("render thread starving");

            new_task_pushed_.wait(lock);
        }

        if (frame_task_queue_.empty())
        {
            return;
        }

        std::swap(frame_tasks, frame_task_queue_.front());

        frame_task_queue_.pop();
    }

    // 2. tell the main thread that new tasks are welcome
    can_push_new_tasks_.notify_all();

    // 3. run tasks
    for (auto &task : frame_tasks)
    {
        task();
    }
}
} // namespace sparkle
