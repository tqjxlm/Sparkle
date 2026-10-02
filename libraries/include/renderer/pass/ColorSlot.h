#pragma once

#include <cstdint>

namespace sparkle
{
// the color slot of each attachment role, fixed across passes: a pass writes a role through its fragment output in
// shaders/include/color_slot.h.slang and has the role's format at the slot of its attachment signature
struct ColorSlot
{
    // also the back buffer's; ImGui writes fragment output 0
    static constexpr uint8_t Screen = 0;
    static constexpr uint8_t SceneColor = 1;
    static constexpr uint8_t GBufferPacked = 2;
    static constexpr uint8_t DepthCopy = 3;
};
} // namespace sparkle
