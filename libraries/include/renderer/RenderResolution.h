#pragma once

#include "core/math/Types.h"

#include <algorithm>
#include <cmath>

namespace sparkle
{
// scene passes render at `scene` (= `output` scaled by render_scale); tone mapping, ui and
// screenshots work at `output`. the present pass maps output onto the RHI-managed surface size.
struct RenderResolution
{
    RenderResolution(const Vector2UInt &output_size, float render_scale)
        : scene(ScaleDimension(output_size.x(), render_scale), ScaleDimension(output_size.y(), render_scale)),
          output(output_size)
    {
    }

    [[nodiscard]] bool NeedUpsample() const
    {
        return scene != output;
    }

    [[nodiscard]] float AspectRatio() const
    {
        return static_cast<float>(output.x()) / static_cast<float>(output.y());
    }

    bool operator==(const RenderResolution &other) const
    {
        return scene == other.scene && output == other.output;
    }

    Vector2UInt scene;
    Vector2UInt output;

private:
    // floor of 2: pixel grids map to uv via 1/(N-1), so a 1-wide dimension divides by zero
    static uint32_t ScaleDimension(uint32_t size, float scale)
    {
        return std::max(2u, static_cast<uint32_t>(std::lround(static_cast<float>(size) * scale)));
    }
};
} // namespace sparkle
