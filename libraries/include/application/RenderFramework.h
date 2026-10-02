#pragma once

#include "core/Event.h"
#include "core/Timer.h"
#include "core/math/Types.h"
#include "renderer/RenderConfig.h"

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace sparkle
{
class Renderer;
class RGTexturePool;
class NativeView;
class Scene;
class UiManager;
struct ThreadTaskQueue;

// a named output the render thread writes to the screenshots directory: a screenshot, a render graph dump, or both
class ScreenshotRequest
{
public:
    explicit ScreenshotRequest(std::string name, bool capture_ui = false, bool dumps_graph = false)
        : name_(std::move(name)), capture_ui_(capture_ui), dumps_graph_(dumps_graph)
    {
    }

    [[nodiscard]] const std::string &GetName() const
    {
        return name_;
    }

    // whether a screenshot shows the ui
    [[nodiscard]] bool CapturesUi() const
    {
        return capture_ui_;
    }

    // whether a screenshot also dumps the render graph of the frame it reads back
    [[nodiscard]] bool DumpsGraph() const
    {
        return dumps_graph_;
    }

    [[nodiscard]] bool IsCompleted() const
    {
        return completed_.load(std::memory_order_acquire);
    }

    void MarkCompleted()
    {
        completed_.store(true, std::memory_order_release);
    }

private:
    std::string name_;
    bool capture_ui_;
    bool dumps_graph_;
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

    // the render graph page: the passes and resources of the frame's graph, the texture render_graph_view shows, and
    // saving a dump. while it is drawn, the render thread publishes the dump of each graph the renderer executes.
    void DrawGraphUi();

    // called by main thread, run on render thread
    void NotifySceneLoaded();

    // Called from main thread. Returns a request handle the caller can poll for completion. With `dump_graph`, the
    // render graph of the frame the screenshot reads back is written to screenshots/<name>.json before it completes.
    [[nodiscard]] std::shared_ptr<ScreenshotRequest> RequestTakeScreenshot(const std::string &name,
                                                                           bool capture_ui = false,
                                                                           bool dump_graph = false);

    // Called from main thread. The next render graph the renderer executes is written to screenshots/<name>.json.
    [[nodiscard]] std::shared_ptr<ScreenshotRequest> RequestGraphDump(const std::string &name);

    // Thread-safe. Returns true when the renderer has accumulated enough samples for a screenshot.
    [[nodiscard]] bool IsReadyForAutoScreenshot() const;

    // Thread-safe. Scene assets loaded (but not necessarily converged) — for tests that need to act
    // before the accumulator caps (e.g. toggling a mode mid-convergence).
    [[nodiscard]] bool IsSceneFullyLoaded() const;

    // render thread only. the images behind render graph transients, kept across renderer recreation
    [[nodiscard]] const RGTexturePool &GetGraphTexturePool() const;

private:
    // called by main thread. converts the ui-space position into render-target space and hands
    // it to the render thread.
    void RequestDebugPoint(const Vector2 &ui_position);

    // render thread only
    void SetDebugPoint(float x, float y);

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

    std::unique_ptr<RGTexturePool> graph_texture_pool_;
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

    Event<> renderer_created_event_;

    std::unique_ptr<EventSubscription> secondary_click_subscription_;

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

    // render thread only: served by the dump of the next render graph the renderer executes
    std::vector<std::function<void(std::shared_ptr<const nlohmann::json>)>> graph_dump_consumers_;

    // the latest dump the render thread published for the render graph page
    std::mutex graph_dump_mutex_;
    std::shared_ptr<const nlohmann::json> graph_dump_;

    // main thread only: the last dump saved from the render graph page
    std::shared_ptr<ScreenshotRequest> saved_graph_dump_;
};
} // namespace sparkle
