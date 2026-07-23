#include "renderer/resource/PathTracingDenoiserInputs.h"

#include "core/Exception.h"
#include "rhi/RHI.h"
#include "rhi/RHIImage.h"

namespace sparkle
{
namespace
{
RHIImage::Attribute MakeImageAttributes(PixelFormat format, const Vector2UInt &size)
{
    return {
        .format = format,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = size.x(),
        .height = size.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        .memory_properties = RHIMemoryProperty::DeviceLocal,
        .mip_levels = 1,
        .msaa_samples = 1,
    };
}
} // namespace

PathTracingDenoiserInputs::PathTracingDenoiserInputs(RHIContext *rhi, Vector2UInt size)
    : rhi_(rhi), size_(std::move(size))
{
    ASSERT(rhi_);
    BindDummies();
}

void PathTracingDenoiserInputs::BindDummies()
{
    auto dummy = [this](PixelFormat format) {
        auto attributes = MakeImageAttributes(format, Vector2UInt{1, 1});
        attributes.memory_properties = RHIMemoryProperty::None;
        return rhi_->GetOrCreateDummyTexture(attributes);
    };

    noisy_radiance_hit_distance_ = dummy(PixelFormat::RGBAFloat);
    normal_view_depth_ = dummy(PixelFormat::RGBAFloat);
    albedo_object_id_ = dummy(PixelFormat::RGBAFloat);
    noisy_specular_radiance_hit_distance_ = dummy(PixelFormat::RGBAFloat);
    motion_hit_metallic_ = dummy(PixelFormat::RGBAFloat16);
    specular_albedo_roughness_ = dummy(PixelFormat::RGBAFloat16);
    radiance_format_ = PixelFormat::Count;
    allocated_ = false;
}

bool PathTracingDenoiserInputs::EnsureAllocated(PixelFormat radiance_format)
{
    ASSERT(radiance_format == PixelFormat::RGBAFloat16 || radiance_format == PixelFormat::RGBAFloat);
    if (allocated_ && radiance_format_ == radiance_format)
    {
        return false;
    }

    noisy_radiance_hit_distance_ = CreateTexture(radiance_format, "GBufferRadiance");
    noisy_specular_radiance_hit_distance_ = CreateTexture(radiance_format, "GBufferRadianceSpecular");

    if (!allocated_)
    {
        normal_view_depth_ = CreateTexture(PixelFormat::RGBAFloat, "GBufferNormalDepth");
        albedo_object_id_ = CreateTexture(PixelFormat::RGBAFloat, "GBufferAlbedoObj");
        motion_hit_metallic_ = CreateTexture(PixelFormat::RGBAFloat16, "GBufferMotion");
        specular_albedo_roughness_ = CreateTexture(PixelFormat::RGBAFloat16, "GBufferSpecAlbedo");
    }

    radiance_format_ = radiance_format;
    allocated_ = true;
    return true;
}

void PathTracingDenoiserInputs::BeginWrite()
{
    ASSERT(allocated_);
    for (const auto &image : {noisy_radiance_hit_distance_, normal_view_depth_, albedo_object_id_, motion_hit_metallic_,
                              noisy_specular_radiance_hit_distance_, specular_albedo_roughness_})
    {
        image->Transition({.target_layout = RHIImageLayout::StorageWrite,
                           .after_stage = RHIPipelineStage::Top,
                           .before_stage = RHIPipelineStage::ComputeShader});
    }
}

RHIDenoiserInputs PathTracingDenoiserInputs::GetInputs(RHIImage *accumulated_radiance) const
{
    return {
        .noisy_radiance_hit_distance = noisy_radiance_hit_distance_.get(),
        .normal_view_depth = normal_view_depth_.get(),
        .albedo_object_id = albedo_object_id_.get(),
        .motion_hit_metallic = motion_hit_metallic_.get(),
        .noisy_specular_radiance_hit_distance = noisy_specular_radiance_hit_distance_.get(),
        .specular_albedo_roughness = specular_albedo_roughness_.get(),
        .accumulated_radiance = accumulated_radiance,
    };
}

RHIResourceRef<RHIImage> PathTracingDenoiserInputs::CreateTexture(PixelFormat format, const std::string &name) const
{
    auto image = rhi_->CreateImage(MakeImageAttributes(format, size_), name);
    image->Transition({.target_layout = RHIImageLayout::Read,
                       .after_stage = RHIPipelineStage::Top,
                       .before_stage = RHIPipelineStage::ComputeShader});
    return image;
}
} // namespace sparkle
