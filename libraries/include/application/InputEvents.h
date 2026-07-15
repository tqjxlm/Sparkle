#pragma once

#include "core/math/Types.h"

#include <variant>

namespace sparkle
{
enum class ClickButton : uint8_t
{
    PrimaryLeft,
    SecondaryRight,
    Count
};

enum class KeyAction : uint8_t
{
    Press,
    Release,
    Count
};

enum class KeyboardModifier : uint8_t
{
    Control = 1u << 0,
    Shift = 1u << 1,
};

enum class PointerDevice : uint8_t
{
    Mouse,
    Touch
};

enum class PointerAction : uint8_t
{
    Down,
    Up,
    Move,
    Cancel
};

// position is in ui space: the coordinate space of imgui's io.DisplaySize.
// on desktop, NativeView::GetWindowScale() maps ui space to render-target pixels.
struct PointerEvent
{
    PointerDevice device = PointerDevice::Mouse;
    PointerAction action = PointerAction::Move;
    uint8_t id = 0;
    ClickButton button = ClickButton::PrimaryLeft;
    uint32_t modifiers = 0;
    Vector2 position = Vector2::Zero();
};

// delta holds raw platform scroll values (glfw offsets or NSEvent deltas).
// sign conventions are resolved when mapping to scene events.
struct ScrollEvent
{
    Vector2 delta = Vector2::Zero();
};

// key is a platform key code, interpreted through the NativeKeyboard enum
struct KeyEvent
{
    int key = 0;
    KeyAction action = KeyAction::Press;
    uint32_t modifiers = 0;
};

struct CharEvent
{
    uint32_t codepoint = 0;
};

using InputEvent = std::variant<PointerEvent, ScrollEvent, KeyEvent, CharEvent>;
} // namespace sparkle
