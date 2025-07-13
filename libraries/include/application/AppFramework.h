#pragma once

#include "application/AppConfig.h"
#include "core/Timer.h"
#include "core/math/Types.h"
#include "renderer/RenderConfig.h"
#include "rhi/RHIConfig.h"

namespace sparkle
{
class NativeView;
class Scene;
class RenderFramework;
class CameraComponent;
class RHIContext;
class UiManager;
class Logger;
class TaskManager;
class MaterialManager;
class EventSubscription;

class AppFramework
{
public:
    enum class ClickButton : uint8_t
    {
        Primary_Left,
        Secondary_Right,
        Count
    };

    enum class KeyAction : uint8_t
    {
        Press,
        Release,
        Count
    };

    enum class KeyboardModifier
    {
        Control = 1u << 0,
        Shift = 1u << 1,
        Count = 0x7fffffff
    };

    AppFramework();

    ~AppFramework();

    bool InitCore(int argc, const char *const argv[]);

    bool Init();

    bool MainLoop();

    void Cleanup();

    static void RequestExit();

    [[nodiscard]] CameraComponent *GetMainCamera() const;

    [[nodiscard]] Vector2 GetLastClickPoint() const
    {
        return {last_x_, last_y_};
    }

    void SetLastClickPoint(float last_x, float last_y)
    {
        last_x_ = last_x;
        last_y_ = last_y;
    }

    [[nodiscard]] const AppConfig &GetAppConfig() const
    {
        return app_config_;
    }

    [[nodiscard]] const RenderConfig &GetRenderConfig() const
    {
        return render_config_;
    }

    NativeView *GetNativeView()
    {
        return view_;
    }

    void SetNativeView(NativeView *window)
    {
        view_ = window;
    }

    [[nodiscard]] const RHIConfig &GetRHIConfig() const
    {
        return rhi_config_;
    }

    [[nodiscard]] RHIContext *GetRHI() const
    {
        return rhi_.get();
    }

    void ResetInputEvents();
    void FrameBufferResizeCallback(int width, int height) const;
    void CursorPositionCallback(double xPos, double yPos);
    void MouseButtonCallback(ClickButton button, KeyAction action, uint32_t mods);
    void ClickCallback();
    void ScrollCallback(double xoffset, double yoffset) const;
    void KeyboardCallback(int key, KeyAction action, bool shift_on) const;
    void CaptureNextFrames(int count);

private:
    void AdvanceFrame(float main_thread_time);

    void DebugNextFrame();

    void MeasurePerformance(float delta_time);

    void GenerateBuiltinUi();

    std::unique_ptr<RenderFramework> render_framework_;

    // resources that are owned by this class
    std::unique_ptr<Scene> main_scene_;
    std::unique_ptr<RHIContext> rhi_;
    std::unique_ptr<UiManager> ui_manager_;

    // core singleton
    std::unique_ptr<Logger> logger_;
    std::unique_ptr<TaskManager> task_manager_;
    std::unique_ptr<MaterialManager> material_manager_;

    // external resources
    NativeView *view_;

    // timer and stats
    Timer frame_timer_;
    float delta_time_;
    uint64_t frame_number_ = 0;
    float last_second_main_thread_time_ = 0.f;
    TimerCaller frame_rate_monitor_;

    // event handler
    bool current_pressing_ = false;
    float last_x_ = -1.f;
    float last_y_ = -1.f;
    Timer click_timer_;
    Timer double_click_timer_;
    Timer double_click_cooldown_;

    // all configs are managed by this class and broadcast to other threads
    AppConfig app_config_;
    RenderConfig render_config_;
    RHIConfig rhi_config_;

    std::unique_ptr<EventSubscription> renderer_created_subscription_;

    bool core_initialized_ = false;
    bool initialized_ = false;
    bool show_settngs_ = false;
    bool renderer_ready_ = false;
};
} // namespace sparkle
