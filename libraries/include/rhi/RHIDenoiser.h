#pragma once

#include "core/math/Types.h"
#include "io/ImageTypes.h"
#include "rhi/RHIResource.h"

namespace sparkle
{
class RHIImage;

enum class RHIPlatformDenoiser : uint8_t
{
    MetalFx
};

struct RHIDenoiserDesc
{
    Vector2UInt input_size;
    Vector2UInt output_size;
    PixelFormat radiance_format;
    uint32_t max_frames_in_flight;
    bool synchronous_initialization = false;
};

struct RHIDenoiserInputs
{
    RHIImage *noisy_radiance_hit_distance;
    RHIImage *normal_view_depth;
    RHIImage *albedo_object_id;
    RHIImage *motion_hit_metallic;
    RHIImage *noisy_specular_radiance_hit_distance;
    RHIImage *specular_albedo_roughness;
    RHIImage *accumulated_radiance;
};

struct RHIDenoiserFrameData
{
    Mat4 view;
    Mat4 projection;
    Vector2 jitter = Vector2::Zero();
    float exposure = 1.f;
    float near_plane = 0.f;
    float far_plane = 0.f;
    uint32_t accumulated_samples = 0;
    uint32_t samples_this_frame = 0;
    uint32_t maximum_samples = 0;
    bool reset_history = false;
    bool final_frame = false;
};

class RHIDenoiser
{
public:
    virtual ~RHIDenoiser() = default;

    [[nodiscard]] virtual bool IsReady() const = 0;
    [[nodiscard]] virtual bool NeedsInputs() const = 0;
    [[nodiscard]] virtual const char *GetName() const = 0;
    [[nodiscard]] virtual RHIResourceRef<RHIImage> GetOutput() const = 0;

    virtual void UpdateFrameData(const RHIDenoiserFrameData &frame) = 0;
    virtual bool Encode(const RHIDenoiserInputs &inputs) = 0;
};
} // namespace sparkle
