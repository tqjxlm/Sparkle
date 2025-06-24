#pragma once

#if FRAMEWORK_ANDROID

#include "application/NativeView.h"
#include "core/Exception.h"

#include "GestureDetector.h"

struct ANativeWindowDeleter
{
    void operator()(ANativeWindow *)
    {
        // TODO(tqjxlm): it should be released somewhere
        // ANativeWindow_release(window);
    }
};

namespace sparkle
{
class AndroidNativeView final : public NativeView
{
public:
    explicit AndroidNativeView(android_app *app_state);

    void SetApp(AppFramework *app)
    {
        app_ = app;
        if (app_state_)
        {
            app_state_->userData = app_;
        }
    }

    void InitGUI(AppFramework *app) override;

    void Cleanup() override;

    bool ShouldClose() override
    {
        return should_close_;
    }

    void Tick() override;

    void SetTitle(const char *) override
    {
    }

    bool CreateVulkanSurface(void *in_instance, void *out_surface) override;

    void GetVulkanRequiredExtensions(std::vector<const char *> &required_extensions) override;

    void GetFrameBufferSize(int &width, int &height) override
    {
        ASSERT(is_valid_);
        width = window_width_;
        height = window_height_;
    }

    void InitUiSystem() override;
    void ShutdownUiSystem() override;
    void TickUiSystem() override;

    void HandleGesture(GameActivityMotionEvent &event, AppFramework *main_app);

    void ResetInputEvents();

    static void OnAppCmd(android_app *app, int32_t cmd);

private:
    void Reset(android_app *app_state);

    void HandleInputEvents();

    android_app *app_state_ = nullptr;
    JavaVM *vm_ = nullptr;
    JNIEnv *jni_ = nullptr;
    std::unique_ptr<ANativeWindow, ANativeWindowDeleter> view_;

    bool should_close_ = false;
    bool ui_enabled_ = false;

    PinchDetector pinch_detector_;
    DragDetector drag_detector_;

    bool is_pinching_ = false;
    bool is_draging_ = false;

    float pinch_length_ = 0;

    int window_width_ = 0;
    int window_height_ = 0;
};
} // namespace sparkle

#endif
