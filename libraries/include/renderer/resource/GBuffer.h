#pragma once

#include "rhi/RHIImage.h"
#include "rhi/RHIRenderTarget.h"

namespace sparkle
{
struct GBuffer
{
    RHIResourceRef<RHIImage> packed_texture;

    RHIRenderTarget::ColorImageArray images;

    void InitRenderResources(RHIContext *rhi, const Vector2UInt &image_size);

    void Transition(const RHIImage::TransitionRequest &request) const;
};

struct CPUGBuffer
{
    // holds one frame's color output. alpha channel: whether this pixel is valid
    std::vector<std::vector<Vector4>> color;

    // holds one frame's normal output
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
        color.resize(height, std::vector<Vector4>(width));
        world_normal.resize(height, std::vector<Vector3>(width));
    }

    void Clear()
    {
        color.clear();
        world_normal.clear();
    }
};
} // namespace sparkle
