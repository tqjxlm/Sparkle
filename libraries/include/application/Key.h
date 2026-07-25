#pragma once

#include <cstdint>

namespace sparkle
{
// Framework-independent key identity. Platform code translates its own key codes into this
// enum at the boundary it produces KeyEvent on, so application and ui code never sees a
// native code and compiles identically on every framework. Letters and digits are
// contiguous so callers can map whole ranges arithmetically.
enum class Key : uint8_t
{
    Unknown = 0,

    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I, // NOLINT
    J,
    K,
    L,
    M,
    N,
    O, // NOLINT
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,

    Escape,
    Enter,
    Tab,
    Space,
    Backspace,

    Up,
    Down,
    Left,
    Right,

    Minus,
    Equal,
    NumpadAdd,
};
} // namespace sparkle
