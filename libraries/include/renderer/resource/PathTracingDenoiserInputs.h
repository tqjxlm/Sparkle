#pragma once

#include "core/math/Types.h"
#include "io/ImageTypes.h"
#include "rhi/RHIDenoiser.h"
#include "rhi/RHIResource.h"

namespace sparkle
{
class RHIContext;
class RHIImage;

class PathTracingDenoiserInputs
{
public:
    PathTracingDenoiserInputs(RHIContext *rhi, Vector2UInt size);

    void BindDummies();
    bool EnsureAllocated(PixelFormat radiance_format);
    void BeginWrite();

    [[nodiscard]] bool IsAllocated() const
    {
        return allocated_;
    }

    [[nodiscard]] RHIDenoiserInputs GetInputs(RHIImage *accumulated_radiance) const;

    [[nodiscard]] const RHIResourceRef<RHIImage> &GetNoisyRadianceHitDistance() const
    {
        return noisy_radiance_hit_distance_;
    }

    [[nodiscard]] const RHIResourceRef<RHIImage> &GetNormalViewDepth() const
    {
        return normal_view_depth_;
    }

    [[nodiscard]] const RHIResourceRef<RHIImage> &GetAlbedoObjectId() const
    {
        return albedo_object_id_;
    }

    [[nodiscard]] const RHIResourceRef<RHIImage> &GetMotionHitMetallic() const
    {
        return motion_hit_metallic_;
    }

    [[nodiscard]] const RHIResourceRef<RHIImage> &GetNoisySpecularRadianceHitDistance() const
    {
        return noisy_specular_radiance_hit_distance_;
    }

    [[nodiscard]] const RHIResourceRef<RHIImage> &GetSpecularAlbedoRoughness() const
    {
        return specular_albedo_roughness_;
    }

private:
    [[nodiscard]] RHIResourceRef<RHIImage> CreateTexture(PixelFormat format, const std::string &name) const;

    RHIContext *rhi_;
    Vector2UInt size_;
    PixelFormat radiance_format_ = PixelFormat::Count;
    bool allocated_ = false;

    RHIResourceRef<RHIImage> noisy_radiance_hit_distance_;
    RHIResourceRef<RHIImage> normal_view_depth_;
    RHIResourceRef<RHIImage> albedo_object_id_;
    RHIResourceRef<RHIImage> motion_hit_metallic_;
    RHIResourceRef<RHIImage> noisy_specular_radiance_hit_distance_;
    RHIResourceRef<RHIImage> specular_albedo_roughness_;
};
} // namespace sparkle
