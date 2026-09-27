#pragma once

#include "core/math/Types.h"
#include "io/ImageTypes.h"
#include "renderer/graph/RenderGraph.h"
#include "rhi/RHIResource.h"

namespace sparkle
{
class RHIImage;

struct DenoiserDesc
{
    Vector2UInt input_size;
    Vector2UInt output_size;
    PixelFormat radiance_format;
    PixelFormat accumulator_format;
    uint32_t max_frames_in_flight;
    bool synchronous_initialization = false;
};

// the path-tracing inputs, as textures of the frame's render graph
struct DenoiserInputs
{
    RGTexture noisy_radiance_hit_distance;
    RGTexture normal_view_depth;
    RGTexture albedo_object_id;
    RGTexture motion_hit_metallic;
    RGTexture noisy_specular_radiance_hit_distance;
    RGTexture specular_albedo_roughness;
    RGTexture accumulated_radiance;
};

struct DenoiserFrameData
{
    Mat4 view;
    Mat4 projection;
    float exposure = 1.f;
    float far_plane = 0.f;
    uint32_t accumulated_samples = 0;
    uint32_t maximum_samples = 0;
    bool reset_history = false;
    bool final_frame = false;
};

// One denoising provider for the GPU path tracer. The provider-neutral path-tracing inputs are
// borrowed for each frame's pass; an implementation owns only its own resources. Providers whose
// implementation needs a platform API live with that backend (see MetalFxDenoiser) but still
// implement this renderer-level interface, so GPURenderer treats every provider alike.
class Denoiser
{
public:
    virtual ~Denoiser() = default;

    [[nodiscard]] virtual bool IsReady() const = 0;
    [[nodiscard]] virtual bool NeedsInputs() const = 0;
    [[nodiscard]] virtual const char *GetName() const = 0;
    // the image the latest AddTo leaves for display
    [[nodiscard]] virtual RHIResourceRef<RHIImage> GetOutput() const = 0;

    virtual void UpdateFrameData(const DenoiserFrameData &frame) = 0;

    // adds the frame's denoising to `graph` as one External pass that reads `inputs` and writes the provider's
    // persistent images, and returns the texture it leaves for display. a provider whose encode fails in that pass
    // stops being ready.
    [[nodiscard]] virtual RGTexture AddTo(RenderGraph &graph, const DenoiserInputs &inputs) = 0;
};
} // namespace sparkle
