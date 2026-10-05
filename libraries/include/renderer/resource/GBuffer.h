#pragma once

#include "core/math/Types.h"

#include <vector>

namespace sparkle
{
struct CPUGBuffer
{
    // holds one frame's color output. alpha channel: whether this pixel is valid
    std::vector<std::vector<Vector4>> color;

    std::vector<std::vector<Vector3>> world_normal;

    [[nodiscard]] bool IsValid(unsigned i, unsigned j) const
    {
        return color[j][i].w() > 0;
    }

    [[nodiscard]] bool IsSky(unsigned i, unsigned j) const
    {
        return IsValid(i, j) && world_normal[j][i].isZero();
    }

    void Resize(unsigned width, unsigned height)
    {
        color.assign(height, std::vector<Vector4>(width));
        world_normal.assign(height, std::vector<Vector3>(width));
    }

    void Clear()
    {
        color.clear();
        world_normal.clear();
    }
};
} // namespace sparkle
