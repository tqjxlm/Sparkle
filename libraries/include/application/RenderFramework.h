#pragma once

#include "core/Event.h"
#include "renderer/RenderConfig.h"

#include <mutex>
#include <queue>
#include <thread>

namespace sparkle
{
class Renderer;
class NativeView;
class Scene;
class UiManager;

class RenderFramework
{
public:
    RenderFramework(NativeView *native_view, RHIContext *rhi, UiManager *ui_manager, Scene *scene);

    ~RenderFramework();

    void StartRenderThread();

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

private:
    void RenderThreadMain();

    [[nodiscard]] bool BeginFrame();

    void EndFrame();

    void RecreateRendererIfNecessary();

    void AdvanceFrame(float render_thread_time);

    void MeasurePerformance(float delta_time);

    static constexpr unsigned RenderTaskBufferSize = 1;
    std::queue<std::vector<std::function<void()>>> frame_task_queue_;

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
};
} // namespace sparkle
