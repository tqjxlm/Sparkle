#if FRAMEWORK_ANDROID

#include "android/AndroidNativeView.h"

#include "android/AndroidFileManager.h"
#include "application/AppFramework.h"
#include "core/Exception.h"
#include "core/task/TaskManager.h"
#include "rhi/VulkanRHI.h"

#include <imgui_impl_android.h>

#include <cfloat>

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
static bool IsTouchToolType(int32_t tool_type)
{
    return tool_type == AMOTION_EVENT_TOOL_TYPE_FINGER || tool_type == AMOTION_EVENT_TOOL_TYPE_UNKNOWN;
}

static void ImGuiHandleAndroidInputEvent(const GameActivityMotionEvent *input_event, const Vector2 &gui_scale)
{
    ImGuiIO &io = ImGui::GetIO();
    constexpr float PixelsPerWheelStep = 80.f;
    constexpr float TouchScrollStartDistance = 8.f;
    static int32_t touch_pointer_id = -1;
    static Vector2 touch_start_pos = Vector2::Zero();
    static Vector2 last_touch_pos = Vector2::Zero();
    static bool touch_scrolling = false;

    int32_t event_action = input_event->action;
    int32_t event_pointer_index =
        (event_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    const auto &axis = input_event->pointers[event_pointer_index];
    int32_t tool_type = axis.toolType;
    uint32_t flags = event_action & AMOTION_EVENT_ACTION_MASK;

    Vector2 event_pos{axis.rawX * gui_scale.x(), axis.rawY * gui_scale.y()};

    switch (tool_type)
    {
    case AMOTION_EVENT_TOOL_TYPE_MOUSE:
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        break;
    case AMOTION_EVENT_TOOL_TYPE_STYLUS:
    case AMOTION_EVENT_TOOL_TYPE_ERASER:
        io.AddMouseSourceEvent(ImGuiMouseSource_Pen);
        break;
    case AMOTION_EVENT_TOOL_TYPE_FINGER:
    default:
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        break;
    }

    switch (flags)
    {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_UP: {
        // Physical mouse buttons (and probably other physical devices) also invoke the actions
        // AMOTION_EVENT_ACTION_DOWN/_UP, but we have to process them separately to identify the actual button pressed.
        // This is done below via AMOTION_EVENT_ACTION_BUTTON_PRESS/_RELEASE. Here, we only process "FINGER" input (and
        // "UNKNOWN", as a fallback).
        if (IsTouchToolType(tool_type))
        {
            if (flags == AMOTION_EVENT_ACTION_DOWN)
            {
                io.AddMousePosEvent(event_pos.x(), event_pos.y());
                touch_pointer_id = axis.id;
                touch_start_pos = event_pos;
                last_touch_pos = event_pos;
                touch_scrolling = false;
            }
            else
            {
                const bool is_active_touch = touch_pointer_id == axis.id;
                if (is_active_touch && !touch_scrolling)
                {
                    // Treat a short single-finger gesture as a tap/click.
                    io.AddMousePosEvent(event_pos.x(), event_pos.y());
                    io.AddMouseButtonEvent(0, true);
                    io.AddMouseButtonEvent(0, false);
                }
                touch_pointer_id = -1;
                touch_scrolling = false;
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            }
        }
        break;
    }
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        if (IsTouchToolType(tool_type))
        {
            touch_pointer_id = -1;
            touch_scrolling = false;
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
        break;
    case AMOTION_EVENT_ACTION_BUTTON_PRESS:
    case AMOTION_EVENT_ACTION_BUTTON_RELEASE: {
        int32_t button_state = input_event->buttonState;
        io.AddMouseButtonEvent(0, (button_state & AMOTION_EVENT_BUTTON_PRIMARY) != 0);
        io.AddMouseButtonEvent(1, (button_state & AMOTION_EVENT_BUTTON_SECONDARY) != 0);
        io.AddMouseButtonEvent(2, (button_state & AMOTION_EVENT_BUTTON_TERTIARY) != 0);
        break;
    }
    case AMOTION_EVENT_ACTION_HOVER_MOVE: // Hovering: Tool moves while NOT pressed (such as a physical mouse)
        io.AddMousePosEvent(event_pos.x(), event_pos.y());
        break;
    case AMOTION_EVENT_ACTION_MOVE: { // Touch pointer moves while DOWN
        const bool is_touch_tool = IsTouchToolType(tool_type);
        const bool is_single_touch_pointer =
            is_touch_tool && input_event->pointerCount == 1 && touch_pointer_id == axis.id;
        if (is_single_touch_pointer)
        {
            auto touch_delta = event_pos - last_touch_pos;
            if (!touch_scrolling)
            {
                io.AddMousePosEvent(event_pos.x(), event_pos.y());
                auto touch_drag = event_pos - touch_start_pos;
                if (touch_drag.squaredNorm() >= TouchScrollStartDistance * TouchScrollStartDistance)
                {
                    touch_scrolling = true;
                }
            }

            if (touch_scrolling && touch_delta.squaredNorm() > 0.f)
            {
                auto wheel_delta = touch_delta / PixelsPerWheelStep;
                io.AddMouseWheelEvent(wheel_delta.x(), wheel_delta.y());
            }

            last_touch_pos = event_pos;
            break;
        }

        io.AddMousePosEvent(event_pos.x(), event_pos.y());
        break;
    }
    case AMOTION_EVENT_ACTION_SCROLL:
        io.AddMouseWheelEvent(GameActivityPointerAxes_getAxisValue(&axis, AMOTION_EVENT_AXIS_HSCROLL),
                              GameActivityPointerAxes_getAxisValue(&axis, AMOTION_EVENT_AXIS_VSCROLL));
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
}

void AndroidNativeView::Tick()
{
    const auto &render_config = app_->GetRenderConfig();
    gui_scale_.x() = render_config.image_width / static_cast<float>(window_width_);
    gui_scale_.y() = render_config.image_height / static_cast<float>(window_height_);

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

        native_view->can_render_ = false;

        break;
    case APP_CMD_DESTROY:
        Log(Info, "APP_CMD_DESTROY");

        // main app will handle the rest in the main loop
        native_view->should_close_ = true;
    default:
        break;
    }
}

void AndroidNativeView::ResetInputEvents()
{
    auto *main_app = static_cast<AppFramework *>(app_state_->userData);
    is_draging_ = false;
    is_pinching_ = false;
    pinch_length_ = 0.f;
    drag_detector_ = DragDetector{};
    pinch_detector_ = PinchDetector{};

    main_app->ResetInputEvents();
}

void AndroidNativeView::HandleGesture(GameActivityMotionEvent &event, AppFramework *main_app)
{
    auto to_app_space = [this](const Vector2 &point) {
        return Vector2{point.x() * gui_scale_.x(), point.y() * gui_scale_.y()};
    };

    GESTURE_STATE drag_state = drag_detector_.Detect(&event);
    GESTURE_STATE pinch_state = pinch_detector_.Detect(&event);

    if (!is_pinching_)
    {
        // Handle drag state
        if (drag_state & GESTURE_STATE_START)
        {
            Vector2 v;
            drag_detector_.GetPointer(v);
            auto app_v = to_app_space(v);
            main_app->CursorPositionCallback(app_v.x(), app_v.y());

            main_app->MouseButtonCallback(AppFramework::ClickButton::Primary_Left, AppFramework::KeyAction::Press, 0);

            is_draging_ = true;
        }
        else if (drag_state & GESTURE_STATE_MOVE)
        {
            ASSERT(is_draging_);

            Vector2 v;
            drag_detector_.GetPointer(v);
            auto app_v = to_app_space(v);
            main_app->CursorPositionCallback(app_v.x(), app_v.y());
        }
        else if (drag_state & GESTURE_STATE_END)
        {
            ASSERT(is_draging_);

            main_app->MouseButtonCallback(AppFramework::ClickButton::Primary_Left, AppFramework::KeyAction::Release, 0);

            is_draging_ = false;
        }
    }

    if (!is_draging_)
    {
        // Handle pinch state
        if (pinch_state & GESTURE_STATE_START)
        {
            // Start new pinch
            Vector2 v1;
            Vector2 v2;
            pinch_detector_.GetPointers(v1, v2);
            pinch_length_ = (v1 - v2).norm();

            is_pinching_ = true;
        }
        else if (pinch_state & GESTURE_STATE_MOVE)
        {
            ASSERT(is_pinching_);

            Vector2 v1;
            Vector2 v2;
            pinch_detector_.GetPointers(v1, v2);
            auto this_pinch_length = (v1 - v2).norm();
            main_app->ScrollCallback(0, (pinch_length_ - this_pinch_length) * 0.1);
            pinch_length_ = this_pinch_length;
        }
        else if (pinch_state & GESTURE_STATE_END)
        {
            ASSERT(is_pinching_);

            pinch_length_ = 0;

            if (pinch_detector_.GetNumPointers() > 0)
            {
                is_draging_ = true;
                Vector2 v;
                pinch_detector_.GetPointer(v);
                auto app_v = to_app_space(v);
                main_app->CursorPositionCallback(app_v.x(), app_v.y());
                main_app->MouseButtonCallback(AppFramework::ClickButton::Primary_Left, AppFramework::KeyAction::Press,
                                              0);
            }

            is_pinching_ = false;
        }
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

    is_valid_ = view_ != nullptr;

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

    // handle key events
    //    for (auto i = 0u; i < input_buffer->keyEventsCount; i++)
    //    {
    //        auto &event = input_buffer->keyEvents[i];
    //    }

    // handle gui first, as it may consume the input events
    for (auto i = 0u; i < input_buffer->motionEventsCount; i++)
    {
        auto &event = input_buffer->motionEvents[i];
        ImGuiHandleAndroidInputEvent(&event, gui_scale_);
    }

    // handle motion events
    for (auto i = 0u; i < input_buffer->motionEventsCount; i++)
    {
        auto &event = input_buffer->motionEvents[i];
        HandleGesture(event, main_app);
    }

    android_app_clear_motion_events(input_buffer);
    android_app_clear_key_events(input_buffer);
}
} // namespace sparkle

#endif
