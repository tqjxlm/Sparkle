#pragma once

#include "core/math/Types.h"
#include "io/ImageTypes.h"
#include "renderer/graph/RenderGraph.h"
#include "rhi/RHIResource.h"

namespace sparkle
{
class RHIImage;

struct UpscalerDesc
{
    Vector2UInt input_size;
    Vector2UInt output_size;
    PixelFormat output_format;
};

// the frame to upscale and the path tracer's primary-hit guides, as textures of the frame's render graph
struct UpscalerInputs
{
    RGTexture color;
    RGTexture normal_view_depth;
    RGTexture motion_hit_metallic;
};

struct UpscalerFrameData
{
    Mat4 projection;
    // the sub-pixel offset `color` was sampled at, in input pixels
    Vector2 jitter = Vector2::Zero();
    float near_plane = 0.f;
    float far_plane = 0.f;
    // seconds since the previous frame
    float delta_time = 0.f;
    bool reset_history = false;
};

// One temporal upscaler for the GPU path tracer: reconstructs a frame at input size to output size. It follows a
// denoiser that serves only scene resolution; a provider that denoises and upscales in one step is a Denoiser instead.
class Upscaler
{
public:
    virtual ~Upscaler() = default;

    [[nodiscard]] virtual bool IsReady() const = 0;
    [[nodiscard]] virtual const char *GetName() const = 0;
    // the image AddTo leaves for display
    [[nodiscard]] virtual RHIResourceRef<RHIImage> GetOutput() const = 0;

    virtual void UpdateFrameData(const UpscalerFrameData &frame) = 0;

    // reallocates the provider's images for new extents and restarts its history. IsReady() tells whether the provider
    // serves them.
    virtual void Resize(const Vector2UInt &input_size, const Vector2UInt &output_size) = 0;

    // adds the frame's upscaling to `graph` and returns the texture it leaves for display at output size. a provider
    // whose encoding fails stops being ready, so the next frame selects its fallback.
    [[nodiscard]] virtual RGTexture AddTo(RenderGraph &graph, const UpscalerInputs &inputs) = 0;
};
} // namespace sparkle
