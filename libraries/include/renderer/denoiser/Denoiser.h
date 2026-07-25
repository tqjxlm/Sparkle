#pragma once

#include "core/math/Types.h"
#include "io/ImageTypes.h"
#include "rhi/RHIResource.h"

namespace sparkle
{
class RHIImage;

struct DenoiserDesc
{
    Vector2UInt input_size;
    Vector2UInt output_size;
    PixelFormat radiance_format;
    uint32_t max_frames_in_flight;
    bool synchronous_initialization = false;
};

struct DenoiserInputs
{
    RHIImage *noisy_radiance_hit_distance;
    RHIImage *normal_view_depth;
    RHIImage *albedo_object_id;
    RHIImage *motion_hit_metallic;
    RHIImage *noisy_specular_radiance_hit_distance;
    RHIImage *specular_albedo_roughness;
    RHIImage *accumulated_radiance;
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
// borrowed for each Encode call; an implementation owns only its own resources. Providers whose
// implementation needs a platform API live with that backend (see MetalFxDenoiser) but still
// implement this renderer-level interface, so GPURenderer treats every provider alike.
class Denoiser
{
public:
    virtual ~Denoiser() = default;

    [[nodiscard]] virtual bool IsReady() const = 0;
    [[nodiscard]] virtual bool NeedsInputs() const = 0;
    [[nodiscard]] virtual const char *GetName() const = 0;
    [[nodiscard]] virtual RHIResourceRef<RHIImage> GetOutput() const = 0;

    virtual void UpdateFrameData(const DenoiserFrameData &frame) = 0;
    virtual bool Encode(const DenoiserInputs &inputs) = 0;
};
} // namespace sparkle
