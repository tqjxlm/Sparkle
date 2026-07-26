#pragma once

#include "application/InputEvents.h"
#include "core/Event.h"
#include "core/Timer.h"

#include <functional>
#include <memory>
#include <vector>

namespace sparkle
{
struct AppConfig;
class UiManager;

// receives raw input events from platform code (or tests, for injection), feeds the ui
// system, and maps them to scene events that any module can subscribe to.
// scene events are suppressed while the ui captures the pointer or keyboard.
//
// there are two tiers of interest:
//   * scene events (On*) are broadcast, for any module that wants to observe an input.
//   * key bindings (BindKey) are arbitrated: one owner per key per InputLayer, and the layers
//     answer in order until one consumes the key.
class InputManager
{
public:
    // returns true when the binding consumed the key, which stops lower layers from seeing it.
    // an owner that is currently irrelevant (a panel that is closed) returns false and lets the
    // key fall through.
    using KeyHandler = std::function<bool()>;

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

    ~InputManager();

    InputManager(const InputManager &) = delete;
    InputManager &operator=(const InputManager &) = delete;

    // the app owns the only instance. cook mode runs without one, so modules that bind their
    // own inputs must tolerate a null instance.
    static InputManager *Instance()
    {
        return instance_;
    }

    // main thread only
    void Push(const InputEvent &event);

    // delivers queued events in push order. called once per frame by the main loop.
    void DispatchPendingEvents();

    void Reset();

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

    // claims a keyboard shortcut in one layer. the slot is freed when the returned subscription
    // dies, and claiming a slot another owner already holds in the same layer asserts — two
    // owners in one layer have no defined order, unlike two owners in different layers.
    // main thread only.
    [[nodiscard]] std::unique_ptr<EventSubscription> BindKey(InputLayer layer, const KeyBinding &binding,
                                                             KeyHandler &&handler);

private:
    // the key slots of every layer. unlike Event, a slot holds a single handler that reports
    // consumption, which is what lets the layers arbitrate. it reuses EventSubscription so a
    // binding's lifetime works exactly like an event subscription's.
    class KeyBindings : public EventListenerBase
    {
    public:
        [[nodiscard]] uint32_t Add(InputLayer layer, const KeyBinding &binding, KeyHandler &&handler);

        // runs the layer's binding for this key, if any. returns true when it consumed the key.
        bool Dispatch(InputLayer layer, const KeyEvent &event);

    protected:
        bool RemoveCallback(uint32_t id) override;

    private:
        struct Slot
        {
            InputLayer layer;
            KeyBinding binding;
            KeyHandler handler;
            uint32_t id;
        };

        // few enough that a linear scan beats hashing
        std::vector<Slot> slots_;
    };

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

    std::shared_ptr<KeyBindings> key_bindings_ = std::make_shared<KeyBindings>();

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

    static InputManager *instance_;
};
} // namespace sparkle
