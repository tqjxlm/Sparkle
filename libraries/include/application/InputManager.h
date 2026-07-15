#pragma once

#include "application/InputEvents.h"
#include "core/Event.h"
#include "core/Timer.h"

#include <vector>

namespace sparkle
{
struct AppConfig;
class UiManager;

// receives raw input events from platform code (or tests, for injection), feeds the ui
// system, and maps them to scene events that any module can subscribe to.
// scene events are suppressed while the ui captures the pointer or keyboard.
class InputManager
{
public:
    // a pointer sequence that starts on the ui stays on the ui until release, even if
    // the pointer leaves it in between. touch consults ShouldConsume only at sequence
    // boundaries (first finger down, last finger up) and reads IsSequenceActive in
    // between, so extra fingers cannot flip the owner mid-sequence.
    // imgui-free so it can be tested without a ui context.
    struct UiCaptureGate
    {
        bool ShouldConsume(PointerAction action, bool ui_wants_pointer)
        {
            if (action == PointerAction::Down)
            {
                sequence_active_ = ui_wants_pointer;
                return sequence_active_;
            }

            if (action == PointerAction::Up || action == PointerAction::Cancel)
            {
                const bool consume = ui_wants_pointer || sequence_active_;
                sequence_active_ = false;
                return consume;
            }

            return ui_wants_pointer || sequence_active_;
        }

        [[nodiscard]] bool IsSequenceActive() const
        {
            return sequence_active_;
        }

        void Reset()
        {
            sequence_active_ = false;
        }

    private:
        bool sequence_active_ = false;
    };

    InputManager(const AppConfig &app_config, UiManager *ui_manager);

    // main thread only
    void Push(const InputEvent &event);

    // delivers queued events in push order. called once per frame by the main loop.
    void DispatchPendingEvents();

    void Reset();

    [[nodiscard]] Vector2 GetPointerPosition() const
    {
        return pointer_position_;
    }

    auto &OnScenePointer()
    {
        return scene_pointer_event_.OnTrigger();
    }

    // drag delta in ui space while the primary pointer is pressed
    auto &OnSceneDrag()
    {
        return scene_drag_event_.OnTrigger();
    }

    // positive amount zooms out, matching OrbitCameraComponent::OnScroll
    auto &OnSceneZoom()
    {
        return scene_zoom_event_.OnTrigger();
    }

    // payload is the finger count of the tap (1 for mouse double click)
    auto &OnSceneDoubleTap()
    {
        return scene_double_tap_event_.OnTrigger();
    }

    // payload is the finger count of a touch tap
    auto &OnSceneTap()
    {
        return scene_tap_event_.OnTrigger();
    }

    auto &OnSceneKey()
    {
        return scene_key_event_.OnTrigger();
    }

private:
    void Process(const PointerEvent &event);
    void Process(const ScrollEvent &event);
    void Process(const KeyEvent &event);
    void Process(const CharEvent &event);

    void ProcessTouch(const PointerEvent &event);

    void FeedUiSystem(const PointerEvent &event);
    void FeedUiSystem(const ScrollEvent &event);
    void FeedUiSystem(const KeyEvent &event);
    void FeedUiSystem(const CharEvent &event);

    void FeedUiSystemTouch(const PointerEvent &event);

    [[nodiscard]] bool UiWantsPointer(const Vector2 *position) const;

    void CancelScenePointer();
    void HandleClick();
    void BeginTouchDrag(uint8_t id, const Vector2 &position);

    const AppConfig &app_config_;
    UiManager *ui_manager_ = nullptr;

    std::vector<InputEvent> pending_events_;

    Event<const PointerEvent &> scene_pointer_event_;
    Event<Vector2> scene_drag_event_;
    Event<float> scene_zoom_event_;
    Event<uint8_t> scene_double_tap_event_;
    Event<uint8_t> scene_tap_event_;
    Event<const KeyEvent &> scene_key_event_;

    UiCaptureGate gate_;

    Vector2 pointer_position_ = Vector2::Zero();
    bool has_pointer_position_ = false;
    bool primary_pressing_ = false;

    Timer click_timer_;
    Timer double_click_timer_;
    Timer double_click_cooldown_;

    struct TouchPointer
    {
        uint8_t id = 0;
        Vector2 position = Vector2::Zero();
        Vector2 down_position = Vector2::Zero();
    };

    struct TouchState
    {
        // insertion-ordered so the pinch pair stays stable while extra fingers come and go
        std::vector<TouchPointer> pointers;
        size_t max_concurrent = 0;
        bool pinching = false;
        bool moved = false;
        float pinch_length = 0.f;
        Timer sequence_timer;
    };

    TouchState touch_;

    struct UiTouchEmulation
    {
        int active_id = -1;
        bool scrolling = false;
        Vector2 start_position = Vector2::Zero();
        Vector2 last_position = Vector2::Zero();
    };

    UiTouchEmulation ui_touch_;
};
} // namespace sparkle
