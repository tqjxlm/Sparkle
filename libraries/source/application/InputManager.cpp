#include "application/InputManager.h"

#include "application/AppConfig.h"
#include "application/UiManager.h"
#include "core/ThreadManager.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <utility>

namespace sparkle
{
constexpr unsigned ClickThresholdMS = 200;
constexpr unsigned DoubleClickThresholdMS = 300;
constexpr unsigned DoubleClickCooldownMS = 300;

// ui-space pixels
constexpr float TouchTapSlop = 16.f;
constexpr float TouchScrollStartDistance = 8.f;
constexpr float PixelsPerWheelStep = 80.f;
constexpr float PinchZoomScale = 0.1f;

InputManager *InputManager::instance_ = nullptr;

#if FRAMEWORK_MACOS
// support keyboards with no escape key: the backspace key stands in for it. only key bindings
// see the substitution, so text editing in the ui still receives a real backspace.
static Key NormalizeBindingKey(Key key)
{
    return key == Key::Backspace ? Key::Escape : key;
}

// covers the keys imgui needs for widget interaction and text editing; printable input
// arrives as CharEvent. only the macos framework needs it: every other framework feeds imgui
// through its own backend (see FeedUiSystem below)
static ImGuiKey ToImGuiKey(Key key)
{
    if (key >= Key::A && key <= Key::Z)
    {
        return static_cast<ImGuiKey>(ImGuiKey_A + (static_cast<int>(key) - static_cast<int>(Key::A)));
    }
    if (key >= Key::Num0 && key <= Key::Num9)
    {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (static_cast<int>(key) - static_cast<int>(Key::Num0)));
    }

    switch (key)
    {
    case Key::Escape:
        return ImGuiKey_Escape;
    case Key::Backspace:
        return ImGuiKey_Backspace;
    case Key::Enter:
        return ImGuiKey_Enter;
    case Key::Tab:
        return ImGuiKey_Tab;
    case Key::Space:
        return ImGuiKey_Space;
    case Key::Up:
        return ImGuiKey_UpArrow;
    case Key::Down:
        return ImGuiKey_DownArrow;
    case Key::Left:
        return ImGuiKey_LeftArrow;
    case Key::Right:
        return ImGuiKey_RightArrow;
    default:
        return ImGuiKey_None;
    }
}
#endif

InputManager::InputManager(const AppConfig &app_config, UiManager *ui_manager)
    : app_config_(app_config), ui_manager_(ui_manager)
{
    ASSERT(!instance_);

    instance_ = this;
}

InputManager::~InputManager()
{
    instance_ = nullptr;
}

uint32_t InputManager::KeyBindings::Add(InputLayer layer, const KeyBinding &binding, KeyHandler &&handler)
{
    const bool claimed = std::ranges::any_of(
        slots_, [layer, &binding](const Slot &slot) { return slot.layer == layer && slot.binding == binding; });
    ASSERT_F(!claimed, "the key is already claimed in this layer. free it before binding again");

    const auto id = AllocateId();

    slots_.push_back({.layer = layer, .binding = binding, .handler = std::move(handler), .id = id});

    return id;
}

bool InputManager::KeyBindings::Dispatch(InputLayer layer, const KeyEvent &event)
{
    auto found = std::ranges::find_if(slots_, [layer, &event](const Slot &slot) {
        return slot.layer == layer && slot.binding.key == event.key && slot.binding.action == event.action &&
               (event.modifiers & slot.binding.modifiers) == slot.binding.modifiers;
    });
    if (found == slots_.end())
    {
        return false;
    }

    // a handler may bind or free a slot, so it runs on a copy: mutating slots_ mid-dispatch
    // must not invalidate the handler being called
    KeyHandler handler = found->handler;

    return handler();
}

bool InputManager::KeyBindings::RemoveCallback(uint32_t id)
{
    return std::erase_if(slots_, [id](const Slot &slot) { return slot.id == id; }) > 0;
}

std::unique_ptr<EventSubscription> InputManager::BindKey(InputLayer layer, const KeyBinding &binding,
                                                         KeyHandler &&handler)
{
    ASSERT(ThreadManager::IsInMainThread());

    const auto id = key_bindings_->Add(layer, binding, std::move(handler));

    return std::make_unique<EventSubscription>(key_bindings_, id);
}

void InputManager::Push(const InputEvent &event)
{
    ASSERT(ThreadManager::IsInMainThread());

    pending_events_.push_back(event);
}

void InputManager::DispatchPendingEvents()
{
    ASSERT(ThreadManager::IsInMainThread());

    // subscribers may push new events. keep those for the next frame.
    std::vector<InputEvent> events;
    events.swap(pending_events_);

    for (const auto &event : events)
    {
        std::visit([this](const auto &typed_event) { Process(typed_event); }, event);
    }
}

void InputManager::Reset()
{
    pending_events_.clear();
    gate_.Reset();
    primary_pressing_ = false;
    has_pointer_position_ = false;
    pointer_position_ = Vector2::Zero();
    touch_ = TouchState{};
    ui_touch_ = UiTouchEmulation{};
}

bool InputManager::UiWantsPointer(const Vector2 *position) const
{
    if (!ui_manager_)
    {
        return false;
    }

    if (ui_manager_->IsHandlingMouseEvent())
    {
        return true;
    }

    return position != nullptr && ui_manager_->IsPointerOverUi(position->x(), position->y());
}

void InputManager::CancelScenePointer()
{
    if (!primary_pressing_)
    {
        return;
    }

    primary_pressing_ = false;

    scene_pointer_event_.Trigger(PointerEvent{.action = PointerAction::Cancel, .position = pointer_position_});
}

void InputManager::HandleClick()
{
    if (double_click_timer_.ElapsedMilliSecond() < DoubleClickThresholdMS)
    {
        if (double_click_cooldown_.ElapsedMilliSecond() > DoubleClickCooldownMS)
        {
            scene_double_tap_event_.Trigger(1);

            double_click_cooldown_.Reset();
        }
    }

    double_click_timer_.Reset();
}

void InputManager::Process(const PointerEvent &event)
{
    FeedUiSystem(event);

    const Vector2 last_position = pointer_position_;
    pointer_position_ = event.position;
    has_pointer_position_ = true;

    if (event.device == PointerDevice::Touch)
    {
        ProcessTouch(event);
        return;
    }

    if (gate_.ShouldConsume(event.action, UiWantsPointer(&event.position)))
    {
        CancelScenePointer();
        return;
    }

    switch (event.action)
    {
    case PointerAction::Down: {
        // control + primary acts as a secondary button, so it must not start a drag
        if (event.button == ClickButton::PrimaryLeft &&
            !(event.modifiers & static_cast<uint32_t>(KeyboardModifier::Control)))
        {
            primary_pressing_ = true;
            click_timer_.Reset();
        }
        scene_pointer_event_.Trigger(event);
        break;
    }
    case PointerAction::Up: {
        const bool was_pressing = primary_pressing_;
        if (event.button == ClickButton::PrimaryLeft)
        {
            primary_pressing_ = false;
        }
        scene_pointer_event_.Trigger(event);
        if (event.button == ClickButton::PrimaryLeft && was_pressing &&
            click_timer_.ElapsedMilliSecond() < ClickThresholdMS)
        {
            HandleClick();
        }
        break;
    }
    case PointerAction::Move: {
        scene_pointer_event_.Trigger(event);
        if (primary_pressing_)
        {
            scene_drag_event_.Trigger(event.position - last_position);
        }
        break;
    }
    case PointerAction::Cancel: {
        CancelScenePointer();
        break;
    }
    default:
        break;
    }
}

void InputManager::BeginTouchDrag(uint8_t id, const Vector2 &position)
{
    primary_pressing_ = true;
    click_timer_.Reset();

    scene_pointer_event_.Trigger(
        PointerEvent{.device = PointerDevice::Touch, .action = PointerAction::Down, .id = id, .position = position});
}

void InputManager::ProcessTouch(const PointerEvent &event)
{
    auto &pointers = touch_.pointers;
    auto found = std::ranges::find_if(pointers, [&event](const auto &pointer) { return pointer.id == event.id; });

    switch (event.action)
    {
    case PointerAction::Down: {
        if (found != pointers.end())
        {
            break;
        }
        pointers.push_back({.id = event.id, .position = event.position, .down_position = event.position});
        touch_.max_concurrent = std::max(touch_.max_concurrent, pointers.size());

        if (pointers.size() == 1)
        {
            // the first finger decides whether the ui or the scene owns the whole sequence
            const bool consumed = gate_.ShouldConsume(PointerAction::Down, UiWantsPointer(&event.position));
            touch_.sequence_timer.Reset();
            touch_.moved = false;
            if (!consumed)
            {
                BeginTouchDrag(event.id, event.position);
            }
        }
        else
        {
            CancelScenePointer();
            if (pointers.size() == 2)
            {
                touch_.pinching = true;
                touch_.pinch_length = (pointers[0].position - pointers[1].position).norm();
            }
        }
        break;
    }
    case PointerAction::Move: {
        if (found == pointers.end())
        {
            break;
        }
        const Vector2 previous = found->position;
        found->position = event.position;

        if ((event.position - found->down_position).norm() > TouchTapSlop)
        {
            touch_.moved = true;
        }

        if (gate_.IsSequenceActive())
        {
            break;
        }

        if (touch_.pinching && pointers.size() >= 2)
        {
            const float new_length = (pointers[0].position - pointers[1].position).norm();
            scene_zoom_event_.Trigger((touch_.pinch_length - new_length) * PinchZoomScale);
            touch_.pinch_length = new_length;
        }
        else if (primary_pressing_ && pointers.size() == 1)
        {
            scene_pointer_event_.Trigger(event);
            scene_drag_event_.Trigger(event.position - previous);
        }
        break;
    }
    case PointerAction::Up: {
        if (found == pointers.end())
        {
            break;
        }
        pointers.erase(found);

        if (pointers.empty())
        {
            const bool consumed = gate_.ShouldConsume(PointerAction::Up, false);
            const bool tapped =
                !consumed && touch_.sequence_timer.ElapsedMilliSecond() < ClickThresholdMS && !touch_.moved;
            if (primary_pressing_)
            {
                primary_pressing_ = false;
                scene_pointer_event_.Trigger(event);
            }
            if (tapped)
            {
                scene_tap_event_.Trigger(static_cast<uint8_t>(touch_.max_concurrent));
                if (touch_.max_concurrent == 1)
                {
                    HandleClick();
                }
            }
            touch_.pinching = false;
            touch_.max_concurrent = 0;
        }
        else if (pointers.size() == 1)
        {
            // the remaining finger resumes a drag
            touch_.pinching = false;
            if (!gate_.IsSequenceActive())
            {
                BeginTouchDrag(pointers[0].id, pointers[0].position);
            }
        }
        else if (touch_.pinching)
        {
            touch_.pinch_length = (pointers[0].position - pointers[1].position).norm();
        }
        break;
    }
    case PointerAction::Cancel: {
        gate_.Reset();
        CancelScenePointer();
        touch_ = TouchState{};
        break;
    }
    default:
        break;
    }
}

void InputManager::Process(const ScrollEvent &event)
{
    FeedUiSystem(event);

    const Vector2 *position = has_pointer_position_ ? &pointer_position_ : nullptr;
    if (gate_.ShouldConsume(PointerAction::Move, UiWantsPointer(position)))
    {
        return;
    }

    // natural scrolling on macos inverts the wheel direction relative to other platforms
    const float direction = app_config_.platform == AppConfig::NativePlatform::MacOS ? -1.f : 1.f;
    scene_zoom_event_.Trigger(direction * static_cast<float>(event.delta.y()));
}

void InputManager::Process(const KeyEvent &event)
{
    FeedUiSystem(event);

    if (ui_manager_ && ui_manager_->IsHandlingKeyboardEvent())
    {
        return;
    }

    KeyEvent binding_event = event;
#if FRAMEWORK_MACOS
    binding_event.key = NormalizeBindingKey(binding_event.key);
#endif

    for (uint8_t layer = 0; layer < static_cast<uint8_t>(InputLayer::Count); layer++)
    {
        if (key_bindings_->Dispatch(static_cast<InputLayer>(layer), binding_event))
        {
            break;
        }
    }
}

void InputManager::Process(const CharEvent &event)
{
    FeedUiSystem(event);
}

void InputManager::FeedUiSystem(const PointerEvent &event)
{
    if (!ui_manager_)
    {
        return;
    }

    if (event.device == PointerDevice::Touch)
    {
        FeedUiSystemTouch(event);
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    const int button = event.button == ClickButton::SecondaryRight ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;

    switch (event.action)
    {
    case PointerAction::Move:
        io.AddMousePosEvent(event.position.x(), event.position.y());
        break;
    case PointerAction::Down:
    case PointerAction::Up:
        io.AddMousePosEvent(event.position.x(), event.position.y());
        io.AddMouseButtonEvent(button, event.action == PointerAction::Down);
        break;
    case PointerAction::Cancel:
        io.AddMouseButtonEvent(button, false);
        break;
    default:
        break;
    }
}

// short single-finger touches read as clicks, longer drags as wheel scrolling, so both
// buttons and scrollable panels work on a touch screen. hover is reset after each
// sequence because a finger, unlike a mouse, leaves the screen.
void InputManager::FeedUiSystemTouch(const PointerEvent &event)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);

    switch (event.action)
    {
    case PointerAction::Down: {
        if (touch_.pointers.empty())
        {
            io.AddMousePosEvent(event.position.x(), event.position.y());
            ui_touch_ = UiTouchEmulation{
                .active_id = event.id, .start_position = event.position, .last_position = event.position};
        }
        else
        {
            ui_touch_ = UiTouchEmulation{};
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
        break;
    }
    case PointerAction::Move: {
        if (std::cmp_not_equal(ui_touch_.active_id, event.id))
        {
            break;
        }

        const Vector2 delta = event.position - ui_touch_.last_position;
        if (!ui_touch_.scrolling)
        {
            io.AddMousePosEvent(event.position.x(), event.position.y());
            if ((event.position - ui_touch_.start_position).squaredNorm() >=
                TouchScrollStartDistance * TouchScrollStartDistance)
            {
                ui_touch_.scrolling = true;
            }
        }

        if (ui_touch_.scrolling && delta.squaredNorm() > 0.f)
        {
            const Vector2 wheel_delta = delta / PixelsPerWheelStep;
            io.AddMouseWheelEvent(wheel_delta.x(), wheel_delta.y());
        }

        ui_touch_.last_position = event.position;
        break;
    }
    case PointerAction::Up:
    case PointerAction::Cancel: {
        if (std::cmp_equal(ui_touch_.active_id, event.id))
        {
            if (event.action == PointerAction::Up && !ui_touch_.scrolling)
            {
                io.AddMousePosEvent(event.position.x(), event.position.y());
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            }
            ui_touch_ = UiTouchEmulation{};
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
        break;
    }
    default:
        break;
    }
}

void InputManager::FeedUiSystem(const ScrollEvent &event)
{
    if (!ui_manager_)
    {
        return;
    }

    ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(event.delta.x()), static_cast<float>(event.delta.y()));
}

void InputManager::FeedUiSystem([[maybe_unused]] const KeyEvent &event)
{
    if (!ui_manager_)
    {
        return;
    }

    // the glfw framework feeds keys through the imgui glfw backend for full key coverage
#if FRAMEWORK_MACOS
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, (event.modifiers & static_cast<uint32_t>(KeyboardModifier::Control)) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (event.modifiers & static_cast<uint32_t>(KeyboardModifier::Shift)) != 0);

    const ImGuiKey key = ToImGuiKey(event.key);
    if (key != ImGuiKey_None)
    {
        io.AddKeyEvent(key, event.action == KeyAction::Press);
    }
#endif
}

void InputManager::FeedUiSystem(const CharEvent &event)
{
    if (!ui_manager_)
    {
        return;
    }

    ImGui::GetIO().AddInputCharacter(event.codepoint);
}
} // namespace sparkle
