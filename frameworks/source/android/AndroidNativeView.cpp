#if FRAMEWORK_ANDROID

#include "android/AndroidNativeView.h"

#include "android/AndroidFileManager.h"
#include "application/AppFramework.h"
#include "application/InputEvents.h"
#include "core/Exception.h"
#include "core/task/TaskManager.h"
#include "rhi/VulkanRHI.h"

#include <imgui_impl_android.h>

extern "C" bool KeyEventFilter(const GameActivityKeyEvent * /*event*/)
{
    return false;
}

extern "C" bool MotionEventFilter(const GameActivityMotionEvent * /*event*/)
{
    return true;
}

namespace sparkle
{
// motion events are treated as touch pointers regardless of tool type.
// ui space is render-target pixels (rawX/rawY scaled by gui_scale).
static void PushMotionEvent(const GameActivityMotionEvent &event, AppFramework *main_app, const Vector2 &gui_scale)
{
    const int32_t action = event.action;
    const uint32_t flags = action & AMOTION_EVENT_ACTION_MASK;
    const int32_t changed_index =
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    auto push = [&](PointerAction pointer_action, int32_t pointer_index) {
        const auto &pointer = event.pointers[pointer_index];
        main_app->PushInputEvent(
            PointerEvent{.device = PointerDevice::Touch,
                         .action = pointer_action,
                         .id = static_cast<uint8_t>(pointer.id),
                         .position = Vector2{pointer.rawX * gui_scale.x(), pointer.rawY * gui_scale.y()}});
    };

    switch (flags)
    {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        push(PointerAction::Down, changed_index);
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        push(PointerAction::Up, changed_index);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        for (uint32_t i = 0; i < event.pointerCount; i++)
        {
            push(PointerAction::Move, static_cast<int32_t>(i));
        }
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        push(PointerAction::Cancel, 0);
        break;
    default:
        break;
    }
}

AndroidNativeView::AndroidNativeView(android_app *app_state)
{
    Reset(app_state);

    auto *activity = app_state->activity;
    static_cast<AndroidFileManager *>(file_manager_.get())
        ->Setup(activity->assetManager, activity->internalDataPath, activity->externalDataPath);
}

void AndroidNativeView::Cleanup()
{
    if (vm_)
    {
        vm_->DetachCurrentThread();
    }

    vm_ = nullptr;
    jni_ = nullptr;
}

void AndroidNativeView::InitGUI(AppFramework *app)
{
    app_ = app;

    headless_ = app->GetAppConfig().headless;
    if (headless_)
    {
        Log(Info, "Headless mode enabled: render without a window.");
        is_valid_ = true;
        can_render_ = true;
    }
}

void AndroidNativeView::Tick()
{
    if (window_width_ > 0 && window_height_ > 0)
    {
        const auto &render_config = app_->GetRenderConfig();
        gui_scale_.x() = render_config.image_width / static_cast<float>(window_width_);
        gui_scale_.y() = render_config.image_height / static_cast<float>(window_height_);
    }

    int ident;
    int events;
    android_poll_source *source;
    while ((ident = ALooper_pollOnce(ShouldClose() ? -1 : 0, nullptr, &events, reinterpret_cast<void **>(&source))) >=
           0)
    {
        if (source != nullptr)
        {
            source->process(app_state_, source);
        }

        if (app_state_->destroyRequested)
        {
            AppFramework::RequestExit();
            break;
        }
    }

    HandleInputEvents();
}

void AndroidNativeView::GetFrameBufferSize(int &width, int &height)
{
    if (headless_)
    {
        const auto &render_config = app_->GetRenderConfig();
        width = static_cast<int>(render_config.image_width);
        height = static_cast<int>(render_config.image_height);
        return;
    }

    ASSERT(is_valid_);
    width = window_width_;
    height = window_height_;
}

void AndroidNativeView::GetVulkanRequiredExtensions(std::vector<const char *> &required_extensions)
{
    required_extensions.push_back("VK_KHR_surface");
    required_extensions.push_back("VK_KHR_android_surface");
}

bool AndroidNativeView::CreateVulkanSurface(void *in_instance, void *out_surface)
{
    if (!view_)
    {
        return false;
    }

    const VkAndroidSurfaceCreateInfoKHR create_info{.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                    .pNext = nullptr,
                                                    .flags = 0,
                                                    .window = view_.get()};

    auto is_success = vkCreateAndroidSurfaceKHR(static_cast<VkInstance>(in_instance), &create_info, nullptr,
                                                static_cast<VkSurfaceKHR *>(out_surface)) == VK_SUCCESS;

    ASSERT(is_success);

    return is_success;
}

void AndroidNativeView::InitUiSystem()
{
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_HSCROLL);
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_VSCROLL);

    ImGui_ImplAndroid_Init(view_.get());
    ui_enabled_ = true;
}

void AndroidNativeView::ShutdownUiSystem()
{
    ImGui_ImplAndroid_Shutdown();
}

void AndroidNativeView::TickUiSystem()
{
    if (can_render_)
    {
        ImGui_ImplAndroid_NewFrame();

        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(app_->GetRenderConfig().image_width),
                                static_cast<float>(app_->GetRenderConfig().image_height));
    }
}

void AndroidNativeView::OnAppCmd(android_app *app_state, int32_t cmd)
{
    auto *main_app = static_cast<AppFramework *>(app_state->userData);
    auto *native_view = static_cast<AndroidNativeView *>(main_app->GetNativeView());
    auto *rhi = main_app->GetRHI();
    if (!native_view)
    {
        return;
    }

    switch (cmd)
    {
    case APP_CMD_START:
        Log(Info, "APP_CMD_START");

        if (!app_state->window)
        {
            return;
        }

        native_view->Reset(app_state);
        break;
    case APP_CMD_INIT_WINDOW:
        Log(Info, "APP_CMD_INIT_WINDOW");

        if (!app_state->window)
        {
            return;
        }

        native_view->Reset(app_state);

        if (native_view->IsHeadless())
        {
            break;
        }

        if (rhi && rhi->IsInitialized())
        {
            TaskManager::RunInRenderThread([rhi]() {
                rhi->RecreateSurface();
                rhi->RecreateSwapChain();
            });
        }

        native_view->can_render_ = true;

        break;
    case APP_CMD_TERM_WINDOW:
        Log(Info, "APP_CMD_TERM_WINDOW");

        main_app->ResetInputEvents();

        if (!native_view->IsHeadless())
        {
            native_view->can_render_ = false;
        }

        break;
    case APP_CMD_DESTROY:
        Log(Info, "APP_CMD_DESTROY");

        // main app will handle the rest in the main loop
        native_view->should_close_ = true;
    default:
        break;
    }
}

void AndroidNativeView::Reset(android_app *app_state)
{
    app_state_ = app_state;

    app_state_->onAppCmd = OnAppCmd;
    app_state_->userData = app_;

    android_app_set_key_event_filter(app_state_, KeyEventFilter);
    android_app_set_motion_event_filter(app_state_, MotionEventFilter);

    vm_ = app_state_->activity->vm;

    vm_->AttachCurrentThread(&jni_, nullptr);

    ASSERT(jni_);

    view_.reset(app_state_->window);

    is_valid_ = headless_ || view_ != nullptr;

    if (is_valid_)
    {
        window_width_ = ANativeWindow_getWidth(view_.get());
        window_height_ = ANativeWindow_getHeight(view_.get());
    }

    if (ui_enabled_)
    {
        // we have to re-init ui system because native window has changed
        InitUiSystem();
    }
}

void AndroidNativeView::HandleInputEvents()
{
    auto *main_app = static_cast<AppFramework *>(app_state_->userData);
    if (!main_app)
    {
        return;
    }

    auto *input_buffer = android_app_swap_input_buffers(app_state_);
    if (!input_buffer)
    {
        return;
    }

    for (auto i = 0u; i < input_buffer->motionEventsCount; i++)
    {
        PushMotionEvent(input_buffer->motionEvents[i], main_app, gui_scale_);
    }

    android_app_clear_motion_events(input_buffer);
    android_app_clear_key_events(input_buffer);
}
} // namespace sparkle

#endif
