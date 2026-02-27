#pragma once

#include "core/Event.h"
#include "core/Timer.h"
#include "renderer/RenderConfig.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace sparkle
{
class Renderer;
class NativeView;
class Scene;
class UiManager;
struct ThreadTaskQueue;

class ScreenshotRequest
{
public:
    explicit ScreenshotRequest(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] const std::string &GetName() const { return name_; }
    [[nodiscard]] bool IsCompleted() const { return completed_.load(std::memory_order_acquire); }
    void MarkCompleted() { completed_.store(true, std::memory_order_release); }

private:
    std::string name_;
    std::atomic<bool> completed_{false};
};

class RenderFramework
{
public:
    RenderFramework(NativeView *native_view, RHIContext *rhi, UiManager *ui_manager, Scene *scene);

    ~RenderFramework();

    void StartRenderThread(const RenderConfig &render_config);

    // called by main thread, run on main thread
    void StopRenderThread();

    // called by main thread, run on main thread
    void PushRenderTasks();

    // called by main thread, run on render thread
    void SetDebugPoint(float x, float y);

    // called by main thread, run on render thread
    void OnFrameBufferResize(int width, int height);

    // called by main thread, run on render thread
    void NewFrame(uint64_t frame_number, const RenderConfig &render_config);

    void WaitUntilIdle();

    void RenderLoop();

    void ConsumeRenderThreadTasks();

    [[nodiscard]] auto &ListenRendererCreatedEvent()
    {
        return renderer_created_event_.OnTrigger();
    }

    void DrawUi();

    // called by main thread, run on render thread
    void NotifySceneLoaded();

    // Called from main thread. Returns a request handle the caller can poll for completion.
    [[nodiscard]] std::shared_ptr<ScreenshotRequest> RequestTakeScreenshot(const std::string &name);

    // Thread-safe. Returns true when the renderer has accumulated enough samples for a screenshot.
    [[nodiscard]] bool IsReadyForAutoScreenshot() const;

private:
    [[nodiscard]] bool IsSceneFullyLoaded() const;

    void ProcessScreenshotRequest();
    void RenderThreadMain();

    [[nodiscard]] bool BeginFrame();

    void EndFrame();

    void RecreateRendererIfNecessary();

    void AdvanceFrame(float render_thread_time);

    void MeasurePerformance(float delta_time);

    static constexpr unsigned MaxBufferedTaskFrames = 1;
    std::queue<std::vector<std::function<void()>>> tasks_per_frame_;
    std::shared_ptr<ThreadTaskQueue> task_queue_;

    std::unique_ptr<Renderer> renderer_;

    NativeView *native_view_ = nullptr;
    RHIContext *rhi_ = nullptr;
    UiManager *ui_manager_ = nullptr;
    Scene *scene_ = nullptr;

    std::thread render_thread_;

    std::condition_variable new_task_pushed_;
    std::condition_variable can_push_new_tasks_;
    std::condition_variable end_of_frame_signal_;
    std::condition_variable render_thread_started_;

    bool should_stop_ = false;
    bool render_loop_started_ = false;

    std::mutex task_queue_mutex_;
    std::mutex thread_mutex_;

    RenderConfig render_config_;

    uint64_t frame_number_ = 0;

    float last_second_render_thread_time_ = 0.f;
    float last_second_gpu_time_ = 0.f;

    Event renderer_created_event_;

    TimerCaller frame_rate_monitor_;

    bool should_capture_ui_ = false;
    std::atomic<bool> screenshot_saving_{false};
    std::string last_saved_screenshot_path_;

    bool scene_loaded_notified_ = false;
    std::atomic<bool> ready_for_auto_screenshot_{false};

    std::mutex screenshot_queue_mutex_;
    std::queue<std::shared_ptr<ScreenshotRequest>> screenshot_queue_;

    // Owned exclusively by the render thread.
    std::shared_ptr<ScreenshotRequest> active_screenshot_;
};
} // namespace sparkle
