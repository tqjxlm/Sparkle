#pragma once

#if FRAMEWORK_ANDROID

#include "application/NativeView.h"
#include "core/Exception.h"

#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <mutex>

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

    [[nodiscard]] bool IsHeadless() const override
    {
        return headless_;
    }

    void Tick() override;

    void SetTitle(const char *) override
    {
    }

    bool CreateVulkanSurface(void *in_instance, void *out_surface) override;

    void GetVulkanRequiredExtensions(std::vector<const char *> &required_extensions) override;

    void GetFrameBufferSize(int &width, int &height) override;

    void InitUiSystem() override;
    void ShutdownUiSystem() override;
    void TickUiSystem() override;

    static void OnAppCmd(android_app *app, int32_t cmd);

private:
    void Reset(android_app *app_state);

    void HandleInputEvents();

    android_app *app_state_ = nullptr;
    JavaVM *vm_ = nullptr;
    JNIEnv *jni_ = nullptr;
    ANativeWindow *view_ = nullptr;
    std::mutex view_mutex_;

    bool should_close_ = false;
    bool ui_enabled_ = false;
    bool headless_ = false;

    int window_width_ = 0;
    int window_height_ = 0;
};
} // namespace sparkle

#endif
