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
} // namespace sparkle
