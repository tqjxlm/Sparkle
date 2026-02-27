#include "renderer/denoiser/ReblurDenoiser.h"

#include "core/math/Utilities.h"
#include "rhi/RHI.h"
#include <algorithm>
#include <cmath>
#include <format>

namespace sparkle
{
namespace
{
class ReblurClassifyTilesShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurClassifyTilesShader, RHIShaderStage::Compute,
                     "shaders/denoiser/reblur/reblur_classify_tiles.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_tiles, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector2 params;
    };
};

class ReblurHitDistanceReconstructionShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurHitDistanceReconstructionShader, RHIShaderStage::Compute,
                     "shaders/denoiser/reblur/reblur_hitdist_reconstruction.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_tiles, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_normal_roughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_diff, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_diff, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector4 params;
    };
};

class ReblurPrePassShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurPrePassShader, RHIShaderStage::Compute, "shaders/denoiser/reblur/reblur_prepass.cs.slang",
                     "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_tiles, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_normal_roughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_diff, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_diff, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec_hit_dist_for_tracking, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector4 params;
    };
};

class ReblurTemporalAccumulationShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurTemporalAccumulationShader, RHIShaderStage::Compute,
                     "shaders/denoiser/reblur/reblur_temporal_accumulation.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_tiles, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_normal_roughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_motion_vectors, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_diff, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec_hit_dist_for_tracking, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prev_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prev_normal_roughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prev_internal_data, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prev_diff_history, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prev_spec_history, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prev_diff_fast_history, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prev_spec_fast_history, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prev_spec_hit_dist_for_tracking, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_data1, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_data2, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_internal_data, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_diff_history, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec_history, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_diff_fast_history, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec_fast_history, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec_hit_dist_for_tracking, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector4 params0;
        Vector4 params1;
    };
};

class ReblurHistoryFixShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurHistoryFixShader, RHIShaderStage::Compute,
                     "shaders/denoiser/reblur/reblur_history_fix.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_tiles, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_normal_roughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_data1, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_diff, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_diff_fast, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec_fast, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec_hit_dist_for_tracking, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_diff, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_diff_fast, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec_fast, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector4 params0;
        Vector4 params1;
    };
};

class ReblurBlurShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurBlurShader, RHIShaderStage::Compute, "shaders/denoiser/reblur/reblur_blur.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_tiles, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_normal_roughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_diff, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_data1, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_prev_view_z, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_diff, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector4 params;
    };
};

class ReblurPostBlurShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurPostBlurShader, RHIShaderStage::Compute, "shaders/denoiser/reblur/reblur_post_blur.cs.slang",
                     "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_tiles, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_normal_roughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_diff, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_prev_normal_roughness, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_diff_history, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec_history, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_denoised_output, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector2 params;
    };
};

class ReblurTemporalStabilizationShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurTemporalStabilizationShader, RHIShaderStage::Compute,
                     "shaders/denoiser/reblur/reblur_temporal_stabilization.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_tiles, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_normal_roughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_data1, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_data2, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_diff_history, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec_history, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec_hit_dist_for_tracking, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prev_diff_stabilized_luma, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prev_spec_stabilized_luma, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_internal_data, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_diff_stabilized_luma, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_spec_stabilized_luma, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(out_denoised_output, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector4 params0;
        Vector4 params1;
    };
};

class ReblurSplitScreenShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurSplitScreenShader, RHIShaderStage::Compute,
                     "shaders/denoiser/reblur/reblur_split_screen.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_diff_noisy, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec_noisy, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_denoised_output, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector2 params;
    };
};

class ReblurValidationShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurValidationShader, RHIShaderStage::Compute,
                     "shaders/denoiser/reblur/reblur_validation.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_tiles, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_data1, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_data2, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_diff, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(in_spec, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_denoised_output, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector4 params;
    };
};

uint32_t GetHitDistanceReconstructionRadius(ReblurDenoiser::HitDistanceReconstructionMode mode)
{
    switch (mode)
    {
    case ReblurDenoiser::HitDistanceReconstructionMode::Area5x5:
        return 2u;
    case ReblurDenoiser::HitDistanceReconstructionMode::Area3x3:
        return 1u;
    case ReblurDenoiser::HitDistanceReconstructionMode::Off:
    default:
        return 0u;
    }
}

float SanitizePrePassRadius(float value, float fallback)
{
    constexpr float MinRadius = 0.0f;
    constexpr float MaxRadius = 4.0f;

    if (!std::isfinite(value))
    {
        return fallback;
    }

    return std::clamp(value, MinRadius, MaxRadius);
}

float SanitizeBlurRadius(float value, float fallback)
{
    constexpr float MinRadius = 0.0f;
    constexpr float MaxRadius = 8.0f;

    if (!std::isfinite(value))
    {
        return fallback;
    }

    return std::clamp(value, MinRadius, MaxRadius);
}

uint32_t SanitizeHistoryFixFrameNum(uint32_t value, uint32_t fallback)
{
    constexpr uint32_t MinFrameNum = 1u;
    constexpr uint32_t MaxFrameNum = 64u;

    if (value < MinFrameNum || value > MaxFrameNum)
    {
        return fallback;
    }

    return value;
}

float SanitizeHistoryFixStride(float value, float fallback)
{
    constexpr float MinStride = 0.0f;
    constexpr float MaxStride = 8.0f;

    if (!std::isfinite(value))
    {
        return fallback;
    }

    return std::clamp(value, MinStride, MaxStride);
}

float SanitizeHistoryFixSigma(float value, float fallback)
{
    constexpr float MinSigma = 0.25f;
    constexpr float MaxSigma = 8.0f;

    if (!std::isfinite(value))
    {
        return fallback;
    }

    return std::clamp(value, MinSigma, MaxSigma);
}

uint32_t SanitizeBlurHistoryMaxFrameNum(uint32_t value, uint32_t fallback)
{
    constexpr uint32_t MinFrameNum = 1u;
    constexpr uint32_t MaxFrameNum = 4096u;

    if (value < MinFrameNum || value > MaxFrameNum)
    {
        return fallback;
    }

    return value;
}

float SanitizeStabilizationStrength(float value, float fallback)
{
    constexpr float MinStrength = 0.0f;
    constexpr float MaxStrength = 1.0f;

    if (!std::isfinite(value))
    {
        return fallback;
    }

    return std::clamp(value, MinStrength, MaxStrength);
}

uint32_t SanitizeStabilizationMaxFrameNum(uint32_t value, uint32_t fallback)
{
    constexpr uint32_t MinFrameNum = 1u;
    constexpr uint32_t MaxFrameNum = 4096u;

    if (value < MinFrameNum || value > MaxFrameNum)
    {
        return fallback;
    }

    return value;
}

ReblurDenoiser::DebugOutputMode SanitizeDebugOutputMode(ReblurDenoiser::DebugOutputMode value)
{
    switch (value)
    {
    case ReblurDenoiser::DebugOutputMode::None:
    case ReblurDenoiser::DebugOutputMode::SplitScreen:
    case ReblurDenoiser::DebugOutputMode::Validation:
        return value;
    default:
        return ReblurDenoiser::DebugOutputMode::None;
    }
}

float SanitizeSplitScreen(float value, float fallback)
{
    if (!std::isfinite(value))
    {
        return fallback;
    }

    return std::clamp(value, 0.0f, 1.0f);
}

bool NearlyEqual(float lhs, float rhs)
{
    return std::abs(lhs - rhs) <= 1e-6f;
}

bool ShouldResetHistoryForSettingsChange(const ReblurDenoiser::Settings &before, const ReblurDenoiser::Settings &after)
{
    return before.hit_distance_reconstruction_mode != after.hit_distance_reconstruction_mode ||
           !NearlyEqual(before.prepass_diffuse_radius, after.prepass_diffuse_radius) ||
           !NearlyEqual(before.prepass_specular_radius, after.prepass_specular_radius) ||
           !NearlyEqual(before.prepass_spec_tracking_radius, after.prepass_spec_tracking_radius) ||
           before.history_fix_frame_num != after.history_fix_frame_num ||
           !NearlyEqual(before.history_fix_base_pixel_stride, after.history_fix_base_pixel_stride) ||
           !NearlyEqual(before.history_fix_sigma_scale, after.history_fix_sigma_scale) ||
           before.history_fix_enable_anti_firefly != after.history_fix_enable_anti_firefly ||
           !NearlyEqual(before.blur_min_radius, after.blur_min_radius) ||
           !NearlyEqual(before.blur_max_radius, after.blur_max_radius) ||
           before.blur_history_max_frame_num != after.blur_history_max_frame_num ||
           before.stabilization_enable != after.stabilization_enable ||
           !NearlyEqual(before.stabilization_strength, after.stabilization_strength) ||
           before.stabilization_max_frame_num != after.stabilization_max_frame_num ||
           before.stabilization_enable_mv_patch != after.stabilization_enable_mv_patch;
}
} // namespace

ReblurDenoiser::ReblurDenoiser(RHIContext *rhi_context) : rhi_(rhi_context)
{
    ASSERT(rhi_);
}

void ReblurDenoiser::CreateTileMaskTexture()
{
    tile_resolution_ = Vector2UInt(utilities::DivideAndRoundUp(image_size_.x(), ReblurTileSize),
                                   utilities::DivideAndRoundUp(image_size_.y(), ReblurTileSize));

    tile_mask_texture_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = PixelFormat::R32_UINT,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = tile_resolution_.x(),
            .height = tile_resolution_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
            .memory_properties = RHIMemoryProperty::DeviceLocal,
            .mip_levels = 1,
            .msaa_samples = 1,
        },
        "ReblurTiles");
}

void ReblurDenoiser::CreateHitDistanceReconstructionTextures()
{
    RHIImage::Attribute attribute{
        .format = PixelFormat::RGBAFloat16,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = image_size_.x(),
        .height = image_size_.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        .memory_properties = RHIMemoryProperty::DeviceLocal,
        .mip_levels = 1,
        .msaa_samples = 1,
    };

    reconstructed_diff_radiance_hitdist_texture_ =
        rhi_->CreateImage(attribute, "ReblurReconstructedDiffRadianceHitDist");
    reconstructed_spec_radiance_hitdist_texture_ =
        rhi_->CreateImage(attribute, "ReblurReconstructedSpecRadianceHitDist");
}

void ReblurDenoiser::CreatePrePassTextures()
{
    RHIImage::Attribute signal_attribute{
        .format = PixelFormat::RGBAFloat16,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = image_size_.x(),
        .height = image_size_.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        .memory_properties = RHIMemoryProperty::DeviceLocal,
        .mip_levels = 1,
        .msaa_samples = 1,
    };

    prepass_diff_radiance_hitdist_texture_ = rhi_->CreateImage(signal_attribute, "ReblurPrePassDiffRadianceHitDist");
    prepass_spec_radiance_hitdist_texture_ = rhi_->CreateImage(signal_attribute, "ReblurPrePassSpecRadianceHitDist");

    RHIImage::Attribute tracking_attribute{
        .format = PixelFormat::R32_FLOAT,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = image_size_.x(),
        .height = image_size_.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        .memory_properties = RHIMemoryProperty::DeviceLocal,
        .mip_levels = 1,
        .msaa_samples = 1,
    };

    spec_hit_distance_for_tracking_texture_ = rhi_->CreateImage(tracking_attribute, "ReblurSpecHitDistForTracking");
}

void ReblurDenoiser::CreateTemporalTextures()
{
    RHIImage::Attribute signal_attribute{
        .format = PixelFormat::RGBAFloat16,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = image_size_.x(),
        .height = image_size_.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        .memory_properties = RHIMemoryProperty::DeviceLocal,
        .mip_levels = 1,
        .msaa_samples = 1,
    };

    data1_texture_ = rhi_->CreateImage(signal_attribute, "ReblurData1");
    data2_texture_ = rhi_->CreateImage(signal_attribute, "ReblurData2");
    temporal_diff_radiance_hitdist_texture_ = rhi_->CreateImage(signal_attribute, "ReblurTemporalDiffRadianceHitDist");
    temporal_spec_radiance_hitdist_texture_ = rhi_->CreateImage(signal_attribute, "ReblurTemporalSpecRadianceHitDist");
    prev_normal_roughness_texture_ = rhi_->CreateImage(signal_attribute, "ReblurPrevNormalRoughness");

    for (uint32_t index = 0u; index < TemporalHistoryPingPongCount; index++)
    {
        diff_history_textures_[index] = rhi_->CreateImage(signal_attribute, std::format("ReblurDiffHistory{}", index));
        spec_history_textures_[index] = rhi_->CreateImage(signal_attribute, std::format("ReblurSpecHistory{}", index));
        diff_fast_history_textures_[index] =
            rhi_->CreateImage(signal_attribute, std::format("ReblurDiffFastHistory{}", index));
        spec_fast_history_textures_[index] =
            rhi_->CreateImage(signal_attribute, std::format("ReblurSpecFastHistory{}", index));
        internal_data_textures_[index] =
            rhi_->CreateImage(signal_attribute, std::format("ReblurInternalData{}", index));
    }

    RHIImage::Attribute tracking_attribute{
        .format = PixelFormat::R32_FLOAT,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = image_size_.x(),
        .height = image_size_.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        .memory_properties = RHIMemoryProperty::DeviceLocal,
        .mip_levels = 1,
        .msaa_samples = 1,
    };

    for (uint32_t index = 0u; index < TemporalHistoryPingPongCount; index++)
    {
        spec_hit_distance_tracking_history_textures_[index] =
            rhi_->CreateImage(tracking_attribute, std::format("ReblurSpecHitDistTrackingHistory{}", index));
    }
}

void ReblurDenoiser::CreateHistoryFixTextures()
{
    RHIImage::Attribute signal_attribute{
        .format = PixelFormat::RGBAFloat16,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = image_size_.x(),
        .height = image_size_.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        .memory_properties = RHIMemoryProperty::DeviceLocal,
        .mip_levels = 1,
        .msaa_samples = 1,
    };

    history_fix_diff_radiance_hitdist_texture_ =
        rhi_->CreateImage(signal_attribute, "ReblurHistoryFixDiffRadianceHitDist");
    history_fix_spec_radiance_hitdist_texture_ =
        rhi_->CreateImage(signal_attribute, "ReblurHistoryFixSpecRadianceHitDist");
}

void ReblurDenoiser::CreateBlurTextures()
{
    RHIImage::Attribute signal_attribute{
        .format = PixelFormat::RGBAFloat16,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = image_size_.x(),
        .height = image_size_.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        .memory_properties = RHIMemoryProperty::DeviceLocal,
        .mip_levels = 1,
        .msaa_samples = 1,
    };

    blur_diff_radiance_hitdist_texture_ = rhi_->CreateImage(signal_attribute, "ReblurBlurDiffRadianceHitDist");
    blur_spec_radiance_hitdist_texture_ = rhi_->CreateImage(signal_attribute, "ReblurBlurSpecRadianceHitDist");

    RHIImage::Attribute view_z_attribute{
        .format = PixelFormat::R32_FLOAT,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = image_size_.x(),
        .height = image_size_.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        .memory_properties = RHIMemoryProperty::DeviceLocal,
        .mip_levels = 1,
        .msaa_samples = 1,
    };

    prev_view_z_texture_ = rhi_->CreateImage(view_z_attribute, "ReblurPrevViewZ");
}

void ReblurDenoiser::CreateStabilizationTextures()
{
    RHIImage::Attribute luma_attribute{
        .format = PixelFormat::R32_FLOAT,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = image_size_.x(),
        .height = image_size_.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
        .memory_properties = RHIMemoryProperty::DeviceLocal,
        .mip_levels = 1,
        .msaa_samples = 1,
    };

    for (uint32_t index = 0u; index < TemporalHistoryPingPongCount; index++)
    {
        diff_stabilized_luma_textures_[index] =
            rhi_->CreateImage(luma_attribute, std::format("ReblurDiffStabilizedLuma{}", index));
        spec_stabilized_luma_textures_[index] =
            rhi_->CreateImage(luma_attribute, std::format("ReblurSpecStabilizedLuma{}", index));
    }
}

void ReblurDenoiser::Initialize(const Vector2UInt &image_size)
{
    ASSERT(image_size.x() > 0 && image_size.y() > 0);

    image_size_ = image_size;

    classify_tiles_shader_ = rhi_->CreateShader<ReblurClassifyTilesShader>();
    hit_distance_reconstruction_shader_ = rhi_->CreateShader<ReblurHitDistanceReconstructionShader>();
    prepass_shader_ = rhi_->CreateShader<ReblurPrePassShader>();
    temporal_accumulation_shader_ = rhi_->CreateShader<ReblurTemporalAccumulationShader>();
    history_fix_shader_ = rhi_->CreateShader<ReblurHistoryFixShader>();
    blur_shader_ = rhi_->CreateShader<ReblurBlurShader>();
    post_blur_shader_ = rhi_->CreateShader<ReblurPostBlurShader>();
    temporal_stabilization_shader_ = rhi_->CreateShader<ReblurTemporalStabilizationShader>();
    split_screen_shader_ = rhi_->CreateShader<ReblurSplitScreenShader>();
    validation_shader_ = rhi_->CreateShader<ReblurValidationShader>();

    classify_tiles_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurClassifyTilesPipeline");
    classify_tiles_pipeline_state_->SetShader<RHIShaderStage::Compute>(classify_tiles_shader_);
    classify_tiles_pipeline_state_->Compile();

    hit_distance_reconstruction_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurHitDistanceReconstructionPipeline");
    hit_distance_reconstruction_pipeline_state_->SetShader<RHIShaderStage::Compute>(
        hit_distance_reconstruction_shader_);
    hit_distance_reconstruction_pipeline_state_->Compile();

    prepass_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurPrePassPipeline");
    prepass_pipeline_state_->SetShader<RHIShaderStage::Compute>(prepass_shader_);
    prepass_pipeline_state_->Compile();

    temporal_accumulation_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurTemporalAccumulationPipeline");
    temporal_accumulation_pipeline_state_->SetShader<RHIShaderStage::Compute>(temporal_accumulation_shader_);
    temporal_accumulation_pipeline_state_->Compile();

    history_fix_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurHistoryFixPipeline");
    history_fix_pipeline_state_->SetShader<RHIShaderStage::Compute>(history_fix_shader_);
    history_fix_pipeline_state_->Compile();

    blur_pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurBlurPipeline");
    blur_pipeline_state_->SetShader<RHIShaderStage::Compute>(blur_shader_);
    blur_pipeline_state_->Compile();

    post_blur_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurPostBlurPipeline");
    post_blur_pipeline_state_->SetShader<RHIShaderStage::Compute>(post_blur_shader_);
    post_blur_pipeline_state_->Compile();

    temporal_stabilization_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurTemporalStabilizationPipeline");
    temporal_stabilization_pipeline_state_->SetShader<RHIShaderStage::Compute>(temporal_stabilization_shader_);
    temporal_stabilization_pipeline_state_->Compile();

    split_screen_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurSplitScreenPipeline");
    split_screen_pipeline_state_->SetShader<RHIShaderStage::Compute>(split_screen_shader_);
    split_screen_pipeline_state_->Compile();

    validation_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurValidationPipeline");
    validation_pipeline_state_->SetShader<RHIShaderStage::Compute>(validation_shader_);
    validation_pipeline_state_->Compile();

    classify_tiles_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurClassifyTilesShader::UniformBufferData),
                                                         .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                         .mem_properties = RHIMemoryProperty::None,
                                                         .is_dynamic = true},
                                                        "ReblurClassifyTilesUniformBuffer");

    hit_distance_reconstruction_uniform_buffer_ =
        rhi_->CreateBuffer({.size = sizeof(ReblurHitDistanceReconstructionShader::UniformBufferData),
                            .usages = RHIBuffer::BufferUsage::UniformBuffer,
                            .mem_properties = RHIMemoryProperty::None,
                            .is_dynamic = true},
                           "ReblurHitDistanceReconstructionUniformBuffer");

    prepass_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurPrePassShader::UniformBufferData),
                                                  .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                  .mem_properties = RHIMemoryProperty::None,
                                                  .is_dynamic = true},
                                                 "ReblurPrePassUniformBuffer");

    temporal_accumulation_uniform_buffer_ =
        rhi_->CreateBuffer({.size = sizeof(ReblurTemporalAccumulationShader::UniformBufferData),
                            .usages = RHIBuffer::BufferUsage::UniformBuffer,
                            .mem_properties = RHIMemoryProperty::None,
                            .is_dynamic = true},
                           "ReblurTemporalAccumulationUniformBuffer");

    history_fix_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurHistoryFixShader::UniformBufferData),
                                                      .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                      .mem_properties = RHIMemoryProperty::None,
                                                      .is_dynamic = true},
                                                     "ReblurHistoryFixUniformBuffer");

    blur_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurBlurShader::UniformBufferData),
                                               .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                               .mem_properties = RHIMemoryProperty::None,
                                               .is_dynamic = true},
                                              "ReblurBlurUniformBuffer");

    post_blur_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurPostBlurShader::UniformBufferData),
                                                    .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                    .mem_properties = RHIMemoryProperty::None,
                                                    .is_dynamic = true},
                                                   "ReblurPostBlurUniformBuffer");

    temporal_stabilization_uniform_buffer_ =
        rhi_->CreateBuffer({.size = sizeof(ReblurTemporalStabilizationShader::UniformBufferData),
                            .usages = RHIBuffer::BufferUsage::UniformBuffer,
                            .mem_properties = RHIMemoryProperty::None,
                            .is_dynamic = true},
                           "ReblurTemporalStabilizationUniformBuffer");

    split_screen_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurSplitScreenShader::UniformBufferData),
                                                       .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                       .mem_properties = RHIMemoryProperty::None,
                                                       .is_dynamic = true},
                                                      "ReblurSplitScreenUniformBuffer");

    validation_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurValidationShader::UniformBufferData),
                                                     .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                     .mem_properties = RHIMemoryProperty::None,
                                                     .is_dynamic = true},
                                                    "ReblurValidationUniformBuffer");

    auto *classify_resources = classify_tiles_pipeline_state_->GetShaderResource<ReblurClassifyTilesShader>();
    classify_resources->ubo().BindResource(classify_tiles_uniform_buffer_);

    auto *hit_distance_reconstruction_resources =
        hit_distance_reconstruction_pipeline_state_->GetShaderResource<ReblurHitDistanceReconstructionShader>();
    hit_distance_reconstruction_resources->ubo().BindResource(hit_distance_reconstruction_uniform_buffer_);

    auto *prepass_resources = prepass_pipeline_state_->GetShaderResource<ReblurPrePassShader>();
    prepass_resources->ubo().BindResource(prepass_uniform_buffer_);

    auto *temporal_accumulation_resources =
        temporal_accumulation_pipeline_state_->GetShaderResource<ReblurTemporalAccumulationShader>();
    temporal_accumulation_resources->ubo().BindResource(temporal_accumulation_uniform_buffer_);

    auto *history_fix_resources = history_fix_pipeline_state_->GetShaderResource<ReblurHistoryFixShader>();
    history_fix_resources->ubo().BindResource(history_fix_uniform_buffer_);

    auto *blur_resources = blur_pipeline_state_->GetShaderResource<ReblurBlurShader>();
    blur_resources->ubo().BindResource(blur_uniform_buffer_);

    auto *post_blur_resources = post_blur_pipeline_state_->GetShaderResource<ReblurPostBlurShader>();
    post_blur_resources->ubo().BindResource(post_blur_uniform_buffer_);

    auto *temporal_stabilization_resources =
        temporal_stabilization_pipeline_state_->GetShaderResource<ReblurTemporalStabilizationShader>();
    temporal_stabilization_resources->ubo().BindResource(temporal_stabilization_uniform_buffer_);

    auto *split_screen_resources = split_screen_pipeline_state_->GetShaderResource<ReblurSplitScreenShader>();
    split_screen_resources->ubo().BindResource(split_screen_uniform_buffer_);

    auto *validation_resources = validation_pipeline_state_->GetShaderResource<ReblurValidationShader>();
    validation_resources->ubo().BindResource(validation_uniform_buffer_);

    classify_tiles_compute_pass_ = rhi_->CreateComputePass("ReblurClassifyTilesComputePass", false);
    hit_distance_reconstruction_compute_pass_ =
        rhi_->CreateComputePass("ReblurHitDistanceReconstructionComputePass", false);
    prepass_compute_pass_ = rhi_->CreateComputePass("ReblurPrePassComputePass", false);
    temporal_accumulation_compute_pass_ = rhi_->CreateComputePass("ReblurTemporalAccumulationComputePass", false);
    history_fix_compute_pass_ = rhi_->CreateComputePass("ReblurHistoryFixComputePass", false);
    blur_compute_pass_ = rhi_->CreateComputePass("ReblurBlurComputePass", false);
    post_blur_compute_pass_ = rhi_->CreateComputePass("ReblurPostBlurComputePass", false);
    temporal_stabilization_compute_pass_ = rhi_->CreateComputePass("ReblurTemporalStabilizationComputePass", false);
    split_screen_compute_pass_ = rhi_->CreateComputePass("ReblurSplitScreenComputePass", false);
    validation_compute_pass_ = rhi_->CreateComputePass("ReblurValidationComputePass", false);

    CreateTileMaskTexture();
    CreateHitDistanceReconstructionTextures();
    CreatePrePassTextures();
    CreateTemporalTextures();
    CreateHistoryFixTextures();
    CreateBlurTextures();
    CreateStabilizationTextures();
    ResetHistory();
}

void ReblurDenoiser::Resize(const Vector2UInt &image_size)
{
    ASSERT(image_size.x() > 0 && image_size.y() > 0);
    image_size_ = image_size;
    CreateTileMaskTexture();
    CreateHitDistanceReconstructionTextures();
    CreatePrePassTextures();
    CreateTemporalTextures();
    CreateHistoryFixTextures();
    CreateBlurTextures();
    CreateStabilizationTextures();
    ResetHistory();
}

void ReblurDenoiser::SetSettings(const Settings &settings)
{
    Settings sanitized_settings = settings;

    switch (sanitized_settings.hit_distance_reconstruction_mode)
    {
    case HitDistanceReconstructionMode::Off:
    case HitDistanceReconstructionMode::Area3x3:
    case HitDistanceReconstructionMode::Area5x5:
        break;
    default:
        sanitized_settings.hit_distance_reconstruction_mode = HitDistanceReconstructionMode::Area3x3;
        break;
    }

    sanitized_settings.prepass_diffuse_radius =
        SanitizePrePassRadius(sanitized_settings.prepass_diffuse_radius, Settings{}.prepass_diffuse_radius);
    sanitized_settings.prepass_specular_radius =
        SanitizePrePassRadius(sanitized_settings.prepass_specular_radius, Settings{}.prepass_specular_radius);
    sanitized_settings.prepass_spec_tracking_radius =
        SanitizePrePassRadius(sanitized_settings.prepass_spec_tracking_radius, Settings{}.prepass_spec_tracking_radius);
    sanitized_settings.history_fix_frame_num =
        SanitizeHistoryFixFrameNum(sanitized_settings.history_fix_frame_num, Settings{}.history_fix_frame_num);
    sanitized_settings.history_fix_base_pixel_stride = SanitizeHistoryFixStride(
        sanitized_settings.history_fix_base_pixel_stride, Settings{}.history_fix_base_pixel_stride);
    sanitized_settings.history_fix_sigma_scale =
        SanitizeHistoryFixSigma(sanitized_settings.history_fix_sigma_scale, Settings{}.history_fix_sigma_scale);
    sanitized_settings.blur_min_radius =
        SanitizeBlurRadius(sanitized_settings.blur_min_radius, Settings{}.blur_min_radius);
    sanitized_settings.blur_max_radius =
        SanitizeBlurRadius(sanitized_settings.blur_max_radius, Settings{}.blur_max_radius);
    sanitized_settings.blur_history_max_frame_num = SanitizeBlurHistoryMaxFrameNum(
        sanitized_settings.blur_history_max_frame_num, Settings{}.blur_history_max_frame_num);
    sanitized_settings.stabilization_strength =
        SanitizeStabilizationStrength(sanitized_settings.stabilization_strength, Settings{}.stabilization_strength);
    sanitized_settings.stabilization_max_frame_num = SanitizeStabilizationMaxFrameNum(
        sanitized_settings.stabilization_max_frame_num, Settings{}.stabilization_max_frame_num);

    if (sanitized_settings.blur_max_radius < sanitized_settings.blur_min_radius)
    {
        sanitized_settings.blur_max_radius = sanitized_settings.blur_min_radius;
    }

    bool should_reset_history = ShouldResetHistoryForSettingsChange(settings_, sanitized_settings);
    settings_ = sanitized_settings;
    if (should_reset_history)
    {
        ResetHistory();
    }
}

void ReblurDenoiser::SetDebugSettings(const DebugSettings &settings)
{
    DebugSettings sanitized_settings = settings;
    sanitized_settings.mode = SanitizeDebugOutputMode(sanitized_settings.mode);
    sanitized_settings.split_screen =
        SanitizeSplitScreen(sanitized_settings.split_screen, DebugSettings{}.split_screen);
    debug_settings_ = sanitized_settings;
}

void ReblurDenoiser::ResetHistory()
{
    temporal_history_read_index_ = 0u;
    temporal_history_valid_ = false;
}

void ReblurDenoiser::Dispatch(const FrontEndInputs &inputs, const RHIResourceRef<RHIImage> &denoised_output)
{
    ASSERT(inputs.normal_roughness);
    ASSERT(inputs.view_z);
    ASSERT(inputs.motion_vectors);
    ASSERT(inputs.diff_radiance_hitdist);
    ASSERT(inputs.spec_radiance_hitdist);
    ASSERT(denoised_output);
    ASSERT(classify_tiles_pipeline_state_);
    ASSERT(classify_tiles_compute_pass_);
    ASSERT(classify_tiles_uniform_buffer_);
    ASSERT(tile_mask_texture_);
    ASSERT(hit_distance_reconstruction_pipeline_state_);
    ASSERT(hit_distance_reconstruction_compute_pass_);
    ASSERT(hit_distance_reconstruction_uniform_buffer_);
    ASSERT(reconstructed_diff_radiance_hitdist_texture_);
    ASSERT(reconstructed_spec_radiance_hitdist_texture_);
    ASSERT(prepass_pipeline_state_);
    ASSERT(prepass_compute_pass_);
    ASSERT(prepass_uniform_buffer_);
    ASSERT(prepass_diff_radiance_hitdist_texture_);
    ASSERT(prepass_spec_radiance_hitdist_texture_);
    ASSERT(spec_hit_distance_for_tracking_texture_);
    ASSERT(temporal_accumulation_pipeline_state_);
    ASSERT(temporal_accumulation_compute_pass_);
    ASSERT(temporal_accumulation_uniform_buffer_);
    ASSERT(data1_texture_);
    ASSERT(data2_texture_);
    ASSERT(temporal_diff_radiance_hitdist_texture_);
    ASSERT(temporal_spec_radiance_hitdist_texture_);
    ASSERT(prev_normal_roughness_texture_);
    ASSERT(diff_history_textures_[0]);
    ASSERT(diff_history_textures_[1]);
    ASSERT(spec_history_textures_[0]);
    ASSERT(spec_history_textures_[1]);
    ASSERT(diff_fast_history_textures_[0]);
    ASSERT(diff_fast_history_textures_[1]);
    ASSERT(spec_fast_history_textures_[0]);
    ASSERT(spec_fast_history_textures_[1]);
    ASSERT(spec_hit_distance_tracking_history_textures_[0]);
    ASSERT(spec_hit_distance_tracking_history_textures_[1]);
    ASSERT(internal_data_textures_[0]);
    ASSERT(internal_data_textures_[1]);
    ASSERT(history_fix_pipeline_state_);
    ASSERT(history_fix_compute_pass_);
    ASSERT(history_fix_uniform_buffer_);
    ASSERT(history_fix_diff_radiance_hitdist_texture_);
    ASSERT(history_fix_spec_radiance_hitdist_texture_);
    ASSERT(blur_pipeline_state_);
    ASSERT(blur_compute_pass_);
    ASSERT(blur_uniform_buffer_);
    ASSERT(blur_diff_radiance_hitdist_texture_);
    ASSERT(blur_spec_radiance_hitdist_texture_);
    ASSERT(prev_view_z_texture_);
    ASSERT(post_blur_pipeline_state_);
    ASSERT(post_blur_compute_pass_);
    ASSERT(post_blur_uniform_buffer_);
    ASSERT(temporal_stabilization_pipeline_state_);
    ASSERT(temporal_stabilization_compute_pass_);
    ASSERT(temporal_stabilization_uniform_buffer_);
    ASSERT(diff_stabilized_luma_textures_[0]);
    ASSERT(diff_stabilized_luma_textures_[1]);
    ASSERT(spec_stabilized_luma_textures_[0]);
    ASSERT(spec_stabilized_luma_textures_[1]);
    ASSERT(split_screen_pipeline_state_);
    ASSERT(split_screen_compute_pass_);
    ASSERT(split_screen_uniform_buffer_);
    ASSERT(validation_pipeline_state_);
    ASSERT(validation_compute_pass_);
    ASSERT(validation_uniform_buffer_);

    Vector2UInt output_size(denoised_output->GetWidth(), denoised_output->GetHeight());
    if (output_size.x() != image_size_.x() || output_size.y() != image_size_.y())
    {
        Resize(output_size);
    }

    ReblurClassifyTilesShader::UniformBufferData classify_ubo{
        .resolution = image_size_,
        .params = Vector2(denoising_range_, 0.0f),
    };
    classify_tiles_uniform_buffer_->Upload(rhi_, &classify_ubo);

    auto *classify_resources = classify_tiles_pipeline_state_->GetShaderResource<ReblurClassifyTilesShader>();
    classify_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
    classify_resources->out_tiles().BindResource(tile_mask_texture_->GetDefaultView(rhi_));

    tile_mask_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                    .after_stage = RHIPipelineStage::Top,
                                    .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(classify_tiles_compute_pass_);
    rhi_->DispatchCompute(classify_tiles_pipeline_state_, {tile_resolution_.x() * 8u, tile_resolution_.y() * 4u, 1u},
                          {8u, 4u, 1u});
    rhi_->EndComputePass(classify_tiles_compute_pass_);

    tile_mask_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::ComputeShader});

    uint32_t reconstruction_radius = GetHitDistanceReconstructionRadius(settings_.hit_distance_reconstruction_mode);
    RHIResourceRef<RHIImage> prepass_input_diff = inputs.diff_radiance_hitdist;
    RHIResourceRef<RHIImage> prepass_input_spec = inputs.spec_radiance_hitdist;
    if (reconstruction_radius > 0u)
    {
        ReblurHitDistanceReconstructionShader::UniformBufferData hit_dist_reconstruction_ubo{
            .resolution = image_size_,
            .params = Vector4(denoising_range_, static_cast<float>(settings_.hit_distance_reconstruction_mode),
                              static_cast<float>(reconstruction_radius), 0.0f),
        };
        hit_distance_reconstruction_uniform_buffer_->Upload(rhi_, &hit_dist_reconstruction_ubo);

        auto *hit_dist_reconstruction_resources =
            hit_distance_reconstruction_pipeline_state_->GetShaderResource<ReblurHitDistanceReconstructionShader>();
        hit_dist_reconstruction_resources->in_tiles().BindResource(tile_mask_texture_->GetDefaultView(rhi_));
        hit_dist_reconstruction_resources->in_normal_roughness().BindResource(
            inputs.normal_roughness->GetDefaultView(rhi_));
        hit_dist_reconstruction_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
        hit_dist_reconstruction_resources->in_diff().BindResource(inputs.diff_radiance_hitdist->GetDefaultView(rhi_));
        hit_dist_reconstruction_resources->in_spec().BindResource(inputs.spec_radiance_hitdist->GetDefaultView(rhi_));
        hit_dist_reconstruction_resources->out_diff().BindResource(
            reconstructed_diff_radiance_hitdist_texture_->GetDefaultView(rhi_));
        hit_dist_reconstruction_resources->out_spec().BindResource(
            reconstructed_spec_radiance_hitdist_texture_->GetDefaultView(rhi_));

        reconstructed_diff_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                  .after_stage = RHIPipelineStage::Top,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
        reconstructed_spec_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                  .after_stage = RHIPipelineStage::Top,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});

        rhi_->BeginComputePass(hit_distance_reconstruction_compute_pass_);
        rhi_->DispatchCompute(hit_distance_reconstruction_pipeline_state_, {image_size_.x(), image_size_.y(), 1u},
                              {16u, 16u, 1u});
        rhi_->EndComputePass(hit_distance_reconstruction_compute_pass_);

        reconstructed_diff_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
        reconstructed_spec_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});

        prepass_input_diff = reconstructed_diff_radiance_hitdist_texture_;
        prepass_input_spec = reconstructed_spec_radiance_hitdist_texture_;
    }

    ReblurPrePassShader::UniformBufferData prepass_ubo{
        .resolution = image_size_,
        .params = Vector4(denoising_range_, settings_.prepass_diffuse_radius, settings_.prepass_specular_radius,
                          settings_.prepass_spec_tracking_radius),
    };
    prepass_uniform_buffer_->Upload(rhi_, &prepass_ubo);

    auto *prepass_resources = prepass_pipeline_state_->GetShaderResource<ReblurPrePassShader>();
    prepass_resources->in_tiles().BindResource(tile_mask_texture_->GetDefaultView(rhi_));
    prepass_resources->in_normal_roughness().BindResource(inputs.normal_roughness->GetDefaultView(rhi_));
    prepass_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
    prepass_resources->in_diff().BindResource(prepass_input_diff->GetDefaultView(rhi_));
    prepass_resources->in_spec().BindResource(prepass_input_spec->GetDefaultView(rhi_));
    prepass_resources->out_diff().BindResource(prepass_diff_radiance_hitdist_texture_->GetDefaultView(rhi_));
    prepass_resources->out_spec().BindResource(prepass_spec_radiance_hitdist_texture_->GetDefaultView(rhi_));
    prepass_resources->out_spec_hit_dist_for_tracking().BindResource(
        spec_hit_distance_for_tracking_texture_->GetDefaultView(rhi_));

    prepass_diff_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                        .after_stage = RHIPipelineStage::Top,
                                                        .before_stage = RHIPipelineStage::ComputeShader});
    prepass_spec_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                        .after_stage = RHIPipelineStage::Top,
                                                        .before_stage = RHIPipelineStage::ComputeShader});
    spec_hit_distance_for_tracking_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                         .after_stage = RHIPipelineStage::Top,
                                                         .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(prepass_compute_pass_);
    rhi_->DispatchCompute(prepass_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {8u, 8u, 1u});
    rhi_->EndComputePass(prepass_compute_pass_);

    prepass_diff_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                        .after_stage = RHIPipelineStage::ComputeShader,
                                                        .before_stage = RHIPipelineStage::ComputeShader});
    prepass_spec_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                        .after_stage = RHIPipelineStage::ComputeShader,
                                                        .before_stage = RHIPipelineStage::ComputeShader});
    spec_hit_distance_for_tracking_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                         .after_stage = RHIPipelineStage::ComputeShader,
                                                         .before_stage = RHIPipelineStage::ComputeShader});

    uint32_t history_read_index = temporal_history_read_index_;
    uint32_t history_write_index = (history_read_index + 1u) % TemporalHistoryPingPongCount;

    ReblurTemporalAccumulationShader::UniformBufferData temporal_accumulation_ubo{
        .resolution = image_size_,
        .params0 = Vector4(denoising_range_, static_cast<float>(settings_.blur_history_max_frame_num), 0.10f, 0.85f),
        .params1 = Vector4(temporal_history_valid_ ? 1.0f : 0.0f, 4.0f, 0.0f, 0.0f),
    };
    temporal_accumulation_uniform_buffer_->Upload(rhi_, &temporal_accumulation_ubo);

    auto *temporal_accumulation_resources =
        temporal_accumulation_pipeline_state_->GetShaderResource<ReblurTemporalAccumulationShader>();
    temporal_accumulation_resources->in_tiles().BindResource(tile_mask_texture_->GetDefaultView(rhi_));
    temporal_accumulation_resources->in_normal_roughness().BindResource(inputs.normal_roughness->GetDefaultView(rhi_));
    temporal_accumulation_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
    temporal_accumulation_resources->in_motion_vectors().BindResource(inputs.motion_vectors->GetDefaultView(rhi_));
    temporal_accumulation_resources->in_diff().BindResource(
        prepass_diff_radiance_hitdist_texture_->GetDefaultView(rhi_));
    temporal_accumulation_resources->in_spec().BindResource(
        prepass_spec_radiance_hitdist_texture_->GetDefaultView(rhi_));
    temporal_accumulation_resources->in_spec_hit_dist_for_tracking().BindResource(
        spec_hit_distance_for_tracking_texture_->GetDefaultView(rhi_));
    temporal_accumulation_resources->prev_view_z().BindResource(prev_view_z_texture_->GetDefaultView(rhi_));
    temporal_accumulation_resources->prev_normal_roughness().BindResource(
        prev_normal_roughness_texture_->GetDefaultView(rhi_));
    temporal_accumulation_resources->prev_internal_data().BindResource(
        internal_data_textures_[history_read_index]->GetDefaultView(rhi_));
    temporal_accumulation_resources->prev_diff_history().BindResource(
        diff_history_textures_[history_read_index]->GetDefaultView(rhi_));
    temporal_accumulation_resources->prev_spec_history().BindResource(
        spec_history_textures_[history_read_index]->GetDefaultView(rhi_));
    temporal_accumulation_resources->prev_diff_fast_history().BindResource(
        diff_fast_history_textures_[history_read_index]->GetDefaultView(rhi_));
    temporal_accumulation_resources->prev_spec_fast_history().BindResource(
        spec_fast_history_textures_[history_read_index]->GetDefaultView(rhi_));
    temporal_accumulation_resources->prev_spec_hit_dist_for_tracking().BindResource(
        spec_hit_distance_tracking_history_textures_[history_read_index]->GetDefaultView(rhi_));
    temporal_accumulation_resources->out_data1().BindResource(data1_texture_->GetDefaultView(rhi_));
    temporal_accumulation_resources->out_data2().BindResource(data2_texture_->GetDefaultView(rhi_));
    temporal_accumulation_resources->out_internal_data().BindResource(
        internal_data_textures_[history_write_index]->GetDefaultView(rhi_));
    temporal_accumulation_resources->out_diff_history().BindResource(
        temporal_diff_radiance_hitdist_texture_->GetDefaultView(rhi_));
    temporal_accumulation_resources->out_spec_history().BindResource(
        temporal_spec_radiance_hitdist_texture_->GetDefaultView(rhi_));
    temporal_accumulation_resources->out_diff_fast_history().BindResource(
        diff_fast_history_textures_[history_write_index]->GetDefaultView(rhi_));
    temporal_accumulation_resources->out_spec_fast_history().BindResource(
        spec_fast_history_textures_[history_write_index]->GetDefaultView(rhi_));
    temporal_accumulation_resources->out_spec_hit_dist_for_tracking().BindResource(
        spec_hit_distance_tracking_history_textures_[history_write_index]->GetDefaultView(rhi_));

    prev_view_z_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                      .after_stage = RHIPipelineStage::Top,
                                      .before_stage = RHIPipelineStage::ComputeShader});
    prev_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                .after_stage = RHIPipelineStage::Top,
                                                .before_stage = RHIPipelineStage::ComputeShader});
    internal_data_textures_[history_read_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                             .after_stage = RHIPipelineStage::Top,
                                                             .before_stage = RHIPipelineStage::ComputeShader});
    diff_history_textures_[history_read_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                            .after_stage = RHIPipelineStage::Top,
                                                            .before_stage = RHIPipelineStage::ComputeShader});
    spec_history_textures_[history_read_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                            .after_stage = RHIPipelineStage::Top,
                                                            .before_stage = RHIPipelineStage::ComputeShader});
    diff_fast_history_textures_[history_read_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                 .after_stage = RHIPipelineStage::Top,
                                                                 .before_stage = RHIPipelineStage::ComputeShader});
    spec_fast_history_textures_[history_read_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                 .after_stage = RHIPipelineStage::Top,
                                                                 .before_stage = RHIPipelineStage::ComputeShader});
    spec_hit_distance_tracking_history_textures_[history_read_index]->Transition(
        {.target_layout = RHIImageLayout::Read,
         .after_stage = RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::ComputeShader});

    data1_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                .after_stage = RHIPipelineStage::Top,
                                .before_stage = RHIPipelineStage::ComputeShader});
    data2_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                .after_stage = RHIPipelineStage::Top,
                                .before_stage = RHIPipelineStage::ComputeShader});
    temporal_diff_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                         .after_stage = RHIPipelineStage::Top,
                                                         .before_stage = RHIPipelineStage::ComputeShader});
    temporal_spec_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                         .after_stage = RHIPipelineStage::Top,
                                                         .before_stage = RHIPipelineStage::ComputeShader});
    internal_data_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                              .after_stage = RHIPipelineStage::Top,
                                                              .before_stage = RHIPipelineStage::ComputeShader});
    diff_fast_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                  .after_stage = RHIPipelineStage::Top,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
    spec_fast_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                  .after_stage = RHIPipelineStage::Top,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
    spec_hit_distance_tracking_history_textures_[history_write_index]->Transition(
        {.target_layout = RHIImageLayout::StorageWrite,
         .after_stage = RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(temporal_accumulation_compute_pass_);
    rhi_->DispatchCompute(temporal_accumulation_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {8u, 8u, 1u});
    rhi_->EndComputePass(temporal_accumulation_compute_pass_);

    data1_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});
    data2_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});
    temporal_diff_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                         .after_stage = RHIPipelineStage::ComputeShader,
                                                         .before_stage = RHIPipelineStage::ComputeShader});
    temporal_spec_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                         .after_stage = RHIPipelineStage::ComputeShader,
                                                         .before_stage = RHIPipelineStage::ComputeShader});
    internal_data_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                              .after_stage = RHIPipelineStage::ComputeShader,
                                                              .before_stage = RHIPipelineStage::ComputeShader});
    diff_fast_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
    spec_fast_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
    spec_hit_distance_tracking_history_textures_[history_write_index]->Transition(
        {.target_layout = RHIImageLayout::Read,
         .after_stage = RHIPipelineStage::ComputeShader,
         .before_stage = RHIPipelineStage::ComputeShader});

    ReblurHistoryFixShader::UniformBufferData history_fix_ubo{
        .resolution = image_size_,
        .params0 = Vector4(denoising_range_, static_cast<float>(settings_.history_fix_frame_num),
                           settings_.history_fix_base_pixel_stride, settings_.history_fix_sigma_scale),
        .params1 = Vector4(settings_.history_fix_enable_anti_firefly ? 1.0f : 0.0f,
                           static_cast<float>(settings_.blur_history_max_frame_num), 0.0f, 0.0f),
    };
    history_fix_uniform_buffer_->Upload(rhi_, &history_fix_ubo);

    auto *history_fix_resources = history_fix_pipeline_state_->GetShaderResource<ReblurHistoryFixShader>();
    RHIResourceRef<RHIImage> history_fix_diff_fast_input = temporal_history_valid_
                                                               ? diff_fast_history_textures_[history_read_index]
                                                               : temporal_diff_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> history_fix_spec_fast_input = temporal_history_valid_
                                                               ? spec_fast_history_textures_[history_read_index]
                                                               : temporal_spec_radiance_hitdist_texture_;
    history_fix_resources->in_tiles().BindResource(tile_mask_texture_->GetDefaultView(rhi_));
    history_fix_resources->in_normal_roughness().BindResource(inputs.normal_roughness->GetDefaultView(rhi_));
    history_fix_resources->in_data1().BindResource(data1_texture_->GetDefaultView(rhi_));
    history_fix_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
    history_fix_resources->in_diff().BindResource(temporal_diff_radiance_hitdist_texture_->GetDefaultView(rhi_));
    history_fix_resources->in_spec().BindResource(temporal_spec_radiance_hitdist_texture_->GetDefaultView(rhi_));
    history_fix_resources->in_diff_fast().BindResource(history_fix_diff_fast_input->GetDefaultView(rhi_));
    history_fix_resources->in_spec_fast().BindResource(history_fix_spec_fast_input->GetDefaultView(rhi_));
    history_fix_resources->in_spec_hit_dist_for_tracking().BindResource(
        spec_hit_distance_tracking_history_textures_[history_write_index]->GetDefaultView(rhi_));
    history_fix_resources->out_diff().BindResource(history_fix_diff_radiance_hitdist_texture_->GetDefaultView(rhi_));
    history_fix_resources->out_spec().BindResource(history_fix_spec_radiance_hitdist_texture_->GetDefaultView(rhi_));
    history_fix_resources->out_diff_fast().BindResource(
        diff_fast_history_textures_[history_write_index]->GetDefaultView(rhi_));
    history_fix_resources->out_spec_fast().BindResource(
        spec_fast_history_textures_[history_write_index]->GetDefaultView(rhi_));

    history_fix_diff_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                            .after_stage = RHIPipelineStage::Top,
                                                            .before_stage = RHIPipelineStage::ComputeShader});
    history_fix_spec_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                            .after_stage = RHIPipelineStage::Top,
                                                            .before_stage = RHIPipelineStage::ComputeShader});
    diff_fast_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                  .after_stage = RHIPipelineStage::Top,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
    spec_fast_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                  .after_stage = RHIPipelineStage::Top,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(history_fix_compute_pass_);
    rhi_->DispatchCompute(history_fix_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {8u, 8u, 1u});
    rhi_->EndComputePass(history_fix_compute_pass_);

    history_fix_diff_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                            .after_stage = RHIPipelineStage::ComputeShader,
                                                            .before_stage = RHIPipelineStage::ComputeShader});
    history_fix_spec_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                            .after_stage = RHIPipelineStage::ComputeShader,
                                                            .before_stage = RHIPipelineStage::ComputeShader});
    diff_fast_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
    spec_fast_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});

    ReblurBlurShader::UniformBufferData blur_ubo{
        .resolution = image_size_,
        .params = Vector4(denoising_range_, settings_.blur_min_radius, settings_.blur_max_radius, 0.0f),
    };
    blur_uniform_buffer_->Upload(rhi_, &blur_ubo);

    auto *blur_resources = blur_pipeline_state_->GetShaderResource<ReblurBlurShader>();
    blur_resources->in_tiles().BindResource(tile_mask_texture_->GetDefaultView(rhi_));
    blur_resources->in_normal_roughness().BindResource(inputs.normal_roughness->GetDefaultView(rhi_));
    blur_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
    blur_resources->in_diff().BindResource(history_fix_diff_radiance_hitdist_texture_->GetDefaultView(rhi_));
    blur_resources->in_spec().BindResource(history_fix_spec_radiance_hitdist_texture_->GetDefaultView(rhi_));
    blur_resources->in_data1().BindResource(data1_texture_->GetDefaultView(rhi_));
    blur_resources->out_prev_view_z().BindResource(prev_view_z_texture_->GetDefaultView(rhi_));
    blur_resources->out_diff().BindResource(blur_diff_radiance_hitdist_texture_->GetDefaultView(rhi_));
    blur_resources->out_spec().BindResource(blur_spec_radiance_hitdist_texture_->GetDefaultView(rhi_));

    prev_view_z_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                      .after_stage = RHIPipelineStage::Top,
                                      .before_stage = RHIPipelineStage::ComputeShader});
    blur_diff_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                     .after_stage = RHIPipelineStage::Top,
                                                     .before_stage = RHIPipelineStage::ComputeShader});
    blur_spec_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                     .after_stage = RHIPipelineStage::Top,
                                                     .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(blur_compute_pass_);
    rhi_->DispatchCompute(blur_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {8u, 8u, 1u});
    rhi_->EndComputePass(blur_compute_pass_);

    prev_view_z_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                      .after_stage = RHIPipelineStage::ComputeShader,
                                      .before_stage = RHIPipelineStage::ComputeShader});
    blur_diff_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                     .after_stage = RHIPipelineStage::ComputeShader,
                                                     .before_stage = RHIPipelineStage::ComputeShader});
    blur_spec_radiance_hitdist_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                     .after_stage = RHIPipelineStage::ComputeShader,
                                                     .before_stage = RHIPipelineStage::ComputeShader});

    ReblurPostBlurShader::UniformBufferData post_blur_ubo{
        .resolution = image_size_,
        .params = Vector2(denoising_range_, 0.0f),
    };
    post_blur_uniform_buffer_->Upload(rhi_, &post_blur_ubo);

    auto *post_blur_resources = post_blur_pipeline_state_->GetShaderResource<ReblurPostBlurShader>();
    post_blur_resources->in_tiles().BindResource(tile_mask_texture_->GetDefaultView(rhi_));
    post_blur_resources->in_normal_roughness().BindResource(inputs.normal_roughness->GetDefaultView(rhi_));
    post_blur_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
    post_blur_resources->in_diff().BindResource(blur_diff_radiance_hitdist_texture_->GetDefaultView(rhi_));
    post_blur_resources->in_spec().BindResource(blur_spec_radiance_hitdist_texture_->GetDefaultView(rhi_));
    post_blur_resources->out_prev_normal_roughness().BindResource(prev_normal_roughness_texture_->GetDefaultView(rhi_));
    post_blur_resources->out_diff_history().BindResource(
        diff_history_textures_[history_write_index]->GetDefaultView(rhi_));
    post_blur_resources->out_spec_history().BindResource(
        spec_history_textures_[history_write_index]->GetDefaultView(rhi_));
    post_blur_resources->out_denoised_output().BindResource(denoised_output->GetDefaultView(rhi_));

    prev_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                .after_stage = RHIPipelineStage::Top,
                                                .before_stage = RHIPipelineStage::ComputeShader});
    diff_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                             .after_stage = RHIPipelineStage::Top,
                                                             .before_stage = RHIPipelineStage::ComputeShader});
    spec_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                             .after_stage = RHIPipelineStage::Top,
                                                             .before_stage = RHIPipelineStage::ComputeShader});
    denoised_output->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                 .after_stage = RHIPipelineStage::Top,
                                 .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(post_blur_compute_pass_);
    rhi_->DispatchCompute(post_blur_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {8u, 8u, 1u});
    rhi_->EndComputePass(post_blur_compute_pass_);

    prev_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                .after_stage = RHIPipelineStage::ComputeShader,
                                                .before_stage = RHIPipelineStage::ComputeShader});
    diff_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                             .after_stage = RHIPipelineStage::ComputeShader,
                                                             .before_stage = RHIPipelineStage::ComputeShader});
    spec_history_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                             .after_stage = RHIPipelineStage::ComputeShader,
                                                             .before_stage = RHIPipelineStage::ComputeShader});
    if (settings_.stabilization_enable)
    {
        ReblurTemporalStabilizationShader::UniformBufferData temporal_stabilization_ubo{
            .resolution = image_size_,
            .params0 = Vector4(denoising_range_, settings_.stabilization_strength,
                               static_cast<float>(settings_.stabilization_max_frame_num),
                               temporal_history_valid_ ? 1.0f : 0.0f),
            .params1 = Vector4(settings_.stabilization_enable_mv_patch ? 1.0f : 0.0f, 0.55f, 2.5f, 0.0f),
        };
        temporal_stabilization_uniform_buffer_->Upload(rhi_, &temporal_stabilization_ubo);

        auto *temporal_stabilization_resources =
            temporal_stabilization_pipeline_state_->GetShaderResource<ReblurTemporalStabilizationShader>();
        temporal_stabilization_resources->in_tiles().BindResource(tile_mask_texture_->GetDefaultView(rhi_));
        temporal_stabilization_resources->in_normal_roughness().BindResource(
            inputs.normal_roughness->GetDefaultView(rhi_));
        temporal_stabilization_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
        temporal_stabilization_resources->in_data1().BindResource(data1_texture_->GetDefaultView(rhi_));
        temporal_stabilization_resources->in_data2().BindResource(data2_texture_->GetDefaultView(rhi_));
        temporal_stabilization_resources->in_diff_history().BindResource(
            diff_history_textures_[history_write_index]->GetDefaultView(rhi_));
        temporal_stabilization_resources->in_spec_history().BindResource(
            spec_history_textures_[history_write_index]->GetDefaultView(rhi_));
        temporal_stabilization_resources->in_spec_hit_dist_for_tracking().BindResource(
            spec_hit_distance_tracking_history_textures_[history_write_index]->GetDefaultView(rhi_));
        temporal_stabilization_resources->prev_diff_stabilized_luma().BindResource(
            diff_stabilized_luma_textures_[history_read_index]->GetDefaultView(rhi_));
        temporal_stabilization_resources->prev_spec_stabilized_luma().BindResource(
            spec_stabilized_luma_textures_[history_read_index]->GetDefaultView(rhi_));
        temporal_stabilization_resources->out_internal_data().BindResource(
            internal_data_textures_[history_write_index]->GetDefaultView(rhi_));
        temporal_stabilization_resources->out_diff_stabilized_luma().BindResource(
            diff_stabilized_luma_textures_[history_write_index]->GetDefaultView(rhi_));
        temporal_stabilization_resources->out_spec_stabilized_luma().BindResource(
            spec_stabilized_luma_textures_[history_write_index]->GetDefaultView(rhi_));
        temporal_stabilization_resources->out_denoised_output().BindResource(denoised_output->GetDefaultView(rhi_));

        diff_stabilized_luma_textures_[history_read_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        spec_stabilized_luma_textures_[history_read_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        internal_data_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                  .after_stage = RHIPipelineStage::Top,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
        diff_stabilized_luma_textures_[history_write_index]->Transition(
            {.target_layout = RHIImageLayout::StorageWrite,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        spec_stabilized_luma_textures_[history_write_index]->Transition(
            {.target_layout = RHIImageLayout::StorageWrite,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        denoised_output->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                     .after_stage = RHIPipelineStage::Top,
                                     .before_stage = RHIPipelineStage::ComputeShader});

        rhi_->BeginComputePass(temporal_stabilization_compute_pass_);
        rhi_->DispatchCompute(temporal_stabilization_pipeline_state_, {image_size_.x(), image_size_.y(), 1u},
                              {8u, 8u, 1u});
        rhi_->EndComputePass(temporal_stabilization_compute_pass_);

        internal_data_textures_[history_write_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
        diff_stabilized_luma_textures_[history_write_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});
        spec_stabilized_luma_textures_[history_write_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});
    }

    if (debug_settings_.mode == DebugOutputMode::SplitScreen)
    {
        ReblurSplitScreenShader::UniformBufferData split_screen_ubo{
            .resolution = image_size_,
            .params = Vector2(denoising_range_, debug_settings_.split_screen),
        };
        split_screen_uniform_buffer_->Upload(rhi_, &split_screen_ubo);

        auto *split_screen_resources = split_screen_pipeline_state_->GetShaderResource<ReblurSplitScreenShader>();
        split_screen_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
        split_screen_resources->in_diff_noisy().BindResource(inputs.diff_radiance_hitdist->GetDefaultView(rhi_));
        split_screen_resources->in_spec_noisy().BindResource(inputs.spec_radiance_hitdist->GetDefaultView(rhi_));
        split_screen_resources->out_denoised_output().BindResource(denoised_output->GetDefaultView(rhi_));

        denoised_output->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                     .after_stage = RHIPipelineStage::ComputeShader,
                                     .before_stage = RHIPipelineStage::ComputeShader});

        rhi_->BeginComputePass(split_screen_compute_pass_);
        rhi_->DispatchCompute(split_screen_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {8u, 8u, 1u});
        rhi_->EndComputePass(split_screen_compute_pass_);
    }
    else if (debug_settings_.mode == DebugOutputMode::Validation)
    {
        ReblurValidationShader::UniformBufferData validation_ubo{
            .resolution = image_size_,
            .params = Vector4(denoising_range_, 0.5f, 0.2f, 0.15f),
        };
        validation_uniform_buffer_->Upload(rhi_, &validation_ubo);

        auto *validation_resources = validation_pipeline_state_->GetShaderResource<ReblurValidationShader>();
        validation_resources->in_tiles().BindResource(tile_mask_texture_->GetDefaultView(rhi_));
        validation_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
        validation_resources->in_data1().BindResource(data1_texture_->GetDefaultView(rhi_));
        validation_resources->in_data2().BindResource(data2_texture_->GetDefaultView(rhi_));
        validation_resources->in_diff().BindResource(diff_history_textures_[history_write_index]->GetDefaultView(rhi_));
        validation_resources->in_spec().BindResource(spec_history_textures_[history_write_index]->GetDefaultView(rhi_));
        validation_resources->out_denoised_output().BindResource(denoised_output->GetDefaultView(rhi_));

        denoised_output->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                     .after_stage = RHIPipelineStage::ComputeShader,
                                     .before_stage = RHIPipelineStage::ComputeShader});

        rhi_->BeginComputePass(validation_compute_pass_);
        rhi_->DispatchCompute(validation_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {8u, 8u, 1u});
        rhi_->EndComputePass(validation_compute_pass_);
    }

    denoised_output->Transition({.target_layout = RHIImageLayout::Read,
                                 .after_stage = RHIPipelineStage::ComputeShader,
                                 .before_stage = RHIPipelineStage::PixelShader});

    temporal_history_read_index_ = history_write_index;
    temporal_history_valid_ = true;
}
} // namespace sparkle
