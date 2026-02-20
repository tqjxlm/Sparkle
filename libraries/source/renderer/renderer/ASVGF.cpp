#include "renderer/renderer/ASVGF.h"

#include "renderer/pass/ToneMappingPass.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

#include <algorithm>

namespace sparkle
{
class ASVGFDebugVisualizeComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ASVGFDebugVisualizeComputeShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/asvgf_debug_visualize.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(noisyTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featureNormalRoughnessTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featureAlbedoMetallicTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featureDepthTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featurePrimitiveIdTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(reprojectionMaskTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(historyMomentsTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(historyColorTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(varianceTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outputImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(reprojectionDebugTexture, RHIShaderResourceReflection::ResourceType::Texture2D)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        uint32_t stage = 0;
        uint32_t view = 0;
        uint32_t history_index = 0;
        uint32_t frame_index = 0;
        uint32_t resolution_x = 1;
        uint32_t resolution_y = 1;
        uint32_t freeze_history = 0;
        uint32_t force_clear_history = 0;
        uint32_t history_cap = 1;
    };
};

class ASVGFReprojectionComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ASVGFReprojectionComputeShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/asvgf_reprojection.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(noisyTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featureNormalRoughnessTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featureDepthTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featurePrimitiveIdTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevHistoryColorTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevHistoryMomentsTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevFeatureNormalRoughnessTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevFeatureDepthTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevFeaturePrimitiveIdTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outHistoryColorImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outHistoryMomentsImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outFeatureNormalRoughnessImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outFeatureDepthImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outFeaturePrimitiveIdImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outReprojectionMaskImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outReprojectionDebugImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outSceneColorImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        alignas(16) Vector4 current_camera_position = Vector4::Zero();
        alignas(16) Vector4 previous_camera_position = Vector4::Zero();
        alignas(16) Vector4 current_lower_left = Vector4::Zero();
        alignas(16) Vector4 current_max_u = Vector4::Zero();
        alignas(16) Vector4 current_max_v = Vector4::Zero();
        alignas(16) Vector4 previous_lower_left = Vector4::Zero();
        alignas(16) Vector4 previous_max_u = Vector4::Zero();
        alignas(16) Vector4 previous_max_v = Vector4::Zero();
        uint32_t resolution_x = 1;
        uint32_t resolution_y = 1;
        uint32_t history_cap = 1;
        uint32_t current_spp = 1;
        uint32_t has_previous_camera = 0;
        uint32_t enable_temporal_accumulation = 0;
    };
};

class ASVGFVarianceComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ASVGFVarianceComputeShader, RHIShaderStage::Compute, "shaders/ray_trace/asvgf_variance.cs.slang",
                     "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(historyMomentsTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featureNormalRoughnessTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featureDepthTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outVarianceImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        uint32_t resolution_x = 1;
        uint32_t resolution_y = 1;
        float depth_sigma_scale = 0.02f;
        float normal_power = 32.f;
    };
};

class ASVGFAtrousComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ASVGFAtrousComputeShader, RHIShaderStage::Compute, "shaders/ray_trace/asvgf_atrous.cs.slang",
                     "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(inputColorTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featureNormalRoughnessTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featureAlbedoMetallicTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featureDepthTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(featurePrimitiveIdTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(varianceTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outFilteredImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        uint32_t resolution_x = 1;
        uint32_t resolution_y = 1;
        uint32_t step_width = 1;
        uint32_t iteration_index = 0;
        float color_sigma_scale = 3.f;
        float depth_sigma_scale = 0.05f;
        float normal_power = 32.f;
        float albedo_sigma = 0.4f;
    };
};

ASVGF::ASVGF(const RenderConfig &render_config, const Vector2UInt &image_size, RHIContext *rhi,
             SceneRenderProxy *scene_render_proxy, const RHIResourceRef<RHIImage> &scene_texture,
             const RHIResourceRef<RHIRenderTarget> &tone_mapping_rt)
    : image_size_(image_size), rhi_(rhi), scene_render_proxy_(scene_render_proxy), scene_texture_(scene_texture),
      tone_mapping_rt_(tone_mapping_rt), last_debug_view_(render_config.asvgf_debug_view),
      last_test_stage_(render_config.asvgf_test_stage), last_enabled_(render_config.asvgf)
{
}

bool ASVGF::RayTraceOutputTextures::IsValid() const
{
    return noisy_texture && feature_normal_roughness_texture && feature_albedo_metallic_texture &&
           feature_depth_texture && feature_primitive_id_texture;
}

void ASVGF::InitFallbackRenderResources()
{
    if (fallback_ray_trace_output_textures_.IsValid())
    {
        return;
    }

    auto create_fallback_texture = [this](PixelFormat format, const std::string &name) {
        return rhi_->CreateImage(
            RHIImage::Attribute{
                .format = format,
                .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                            .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                            .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
                .width = 1,
                .height = 1,
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
                .mip_levels = 1,
                .msaa_samples = 1,
            },
            name);
    };

    fallback_ray_trace_output_textures_ = {
        .noisy_texture = create_fallback_texture(PixelFormat::RGBAFloat, "ASVGFFallbackNoisy"),
        .feature_normal_roughness_texture =
            create_fallback_texture(PixelFormat::RGBAFloat16, "ASVGFFallbackNormalRoughness"),
        .feature_albedo_metallic_texture =
            create_fallback_texture(PixelFormat::RGBAFloat16, "ASVGFFallbackAlbedoMetallic"),
        .feature_depth_texture = create_fallback_texture(PixelFormat::R32_FLOAT, "ASVGFFallbackDepth"),
        .feature_primitive_id_texture = create_fallback_texture(PixelFormat::R32_UINT, "ASVGFFallbackPrimitiveId"),
    };
}

void ASVGF::InitRenderResources(const RenderConfig &render_config)
{
    if (HasPipelineResources())
    {
        return;
    }

    auto create_texture = [this](PixelFormat format, const std::string &name) {
        return rhi_->CreateImage(
            RHIImage::Attribute{
                .format = format,
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
            },
            name);
    };

    noisy_texture_ = create_texture(PixelFormat::RGBAFloat, "ASVGFNoisyTexture");
    feature_normal_roughness_texture_ = create_texture(PixelFormat::RGBAFloat16, "ASVGFFeatureNormalRoughness");
    feature_albedo_metallic_texture_ = create_texture(PixelFormat::RGBAFloat16, "ASVGFFeatureAlbedoMetallic");
    feature_depth_texture_ = create_texture(PixelFormat::R32_FLOAT, "ASVGFFeatureDepth");
    feature_primitive_id_texture_ = create_texture(PixelFormat::R32_UINT, "ASVGFFeaturePrimitiveId");
    history_feature_normal_roughness_texture_[0] =
        create_texture(PixelFormat::RGBAFloat16, "ASVGFHistoryFeatureNormalRoughness0");
    history_feature_normal_roughness_texture_[1] =
        create_texture(PixelFormat::RGBAFloat16, "ASVGFHistoryFeatureNormalRoughness1");
    history_feature_depth_texture_[0] = create_texture(PixelFormat::R32_FLOAT, "ASVGFHistoryDepth0");
    history_feature_depth_texture_[1] = create_texture(PixelFormat::R32_FLOAT, "ASVGFHistoryDepth1");
    history_feature_primitive_id_texture_[0] = create_texture(PixelFormat::R32_UINT, "ASVGFHistoryPrimitiveId0");
    history_feature_primitive_id_texture_[1] = create_texture(PixelFormat::R32_UINT, "ASVGFHistoryPrimitiveId1");
    history_color_texture_[0] = create_texture(PixelFormat::RGBAFloat, "ASVGFHistoryColor0");
    history_color_texture_[1] = create_texture(PixelFormat::RGBAFloat, "ASVGFHistoryColor1");
    history_moments_texture_[0] = create_texture(PixelFormat::RGBAFloat16, "ASVGFHistoryMoments0");
    history_moments_texture_[1] = create_texture(PixelFormat::RGBAFloat16, "ASVGFHistoryMoments1");
    variance_texture_ = create_texture(PixelFormat::R32_FLOAT, "ASVGFVariance");
    reprojection_mask_texture_ = create_texture(PixelFormat::R32_FLOAT, "ASVGFReprojectionMask");
    reprojection_debug_texture_ = create_texture(PixelFormat::RGBAFloat16, "ASVGFReprojectionDebug");
    atrous_ping_pong_texture_[0] = create_texture(PixelFormat::RGBAFloat16, "ASVGFAtrousPing");
    atrous_ping_pong_texture_[1] = create_texture(PixelFormat::RGBAFloat16, "ASVGFAtrousPong");
    debug_texture_ = create_texture(PixelFormat::RGBAFloat, "ASVGFDebug");

    debug_tone_mapping_pass_ =
        PipelinePass::Create<ToneMappingPass>(render_config, rhi_, debug_texture_, tone_mapping_rt_);

    reprojection_uniform_buffer_ =
        rhi_->CreateBuffer({.size = sizeof(ASVGFReprojectionComputeShader::UniformBufferData),
                            .usages = RHIBuffer::BufferUsage::UniformBuffer,
                            .mem_properties = RHIMemoryProperty::None,
                            .is_dynamic = true},
                           "ASVGFReprojectionUniformBuffer");
    reprojection_shader_ = rhi_->CreateShader<ASVGFReprojectionComputeShader>();
    reprojection_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ASVGFReprojectionPSO");
    reprojection_pipeline_state_->SetShader<RHIShaderStage::Compute>(reprojection_shader_);
    reprojection_pipeline_state_->Compile();

    variance_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ASVGFVarianceComputeShader::UniformBufferData),
                                                   .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                   .mem_properties = RHIMemoryProperty::None,
                                                   .is_dynamic = true},
                                                  "ASVGFVarianceUniformBuffer");
    variance_shader_ = rhi_->CreateShader<ASVGFVarianceComputeShader>();
    variance_pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ASVGFVariancePSO");
    variance_pipeline_state_->SetShader<RHIShaderStage::Compute>(variance_shader_);
    variance_pipeline_state_->Compile();

    atrous_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ASVGFAtrousComputeShader::UniformBufferData),
                                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                 .mem_properties = RHIMemoryProperty::None,
                                                 .is_dynamic = true},
                                                "ASVGFAtrousUniformBuffer");
    atrous_shader_ = rhi_->CreateShader<ASVGFAtrousComputeShader>();
    for (uint32_t iteration = 0; iteration < atrous_pipeline_states_.size(); ++iteration)
    {
        atrous_pipeline_states_[iteration] = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute,
                                                                       fmt::format("ASVGFAtrousPSO{}", iteration));
        atrous_pipeline_states_[iteration]->SetShader<RHIShaderStage::Compute>(atrous_shader_);
        atrous_pipeline_states_[iteration]->Compile();
    }

    debug_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ASVGFDebugVisualizeComputeShader::UniformBufferData),
                                                .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                .mem_properties = RHIMemoryProperty::None,
                                                .is_dynamic = true},
                                               "ASVGFDebugUniformBuffer");
    debug_shader_ = rhi_->CreateShader<ASVGFDebugVisualizeComputeShader>();
    debug_pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ASVGFDebugPSO");
    debug_pipeline_state_->SetShader<RHIShaderStage::Compute>(debug_shader_);
    debug_pipeline_state_->Compile();

    auto *reprojection_resources = reprojection_pipeline_state_->GetShaderResource<ASVGFReprojectionComputeShader>();
    reprojection_resources->ubo().BindResource(reprojection_uniform_buffer_);
    reprojection_resources->noisyTexture().BindResource(noisy_texture_->GetDefaultView(rhi_));
    reprojection_resources->featureNormalRoughnessTexture().BindResource(
        feature_normal_roughness_texture_->GetDefaultView(rhi_));
    reprojection_resources->featureDepthTexture().BindResource(feature_depth_texture_->GetDefaultView(rhi_));
    reprojection_resources->featurePrimitiveIdTexture().BindResource(
        feature_primitive_id_texture_->GetDefaultView(rhi_));
    reprojection_resources->outReprojectionMaskImage().BindResource(reprojection_mask_texture_->GetDefaultView(rhi_));
    reprojection_resources->outReprojectionDebugImage().BindResource(reprojection_debug_texture_->GetDefaultView(rhi_));
    reprojection_resources->outSceneColorImage().BindResource(scene_texture_->GetDefaultView(rhi_));

    auto *variance_resources = variance_pipeline_state_->GetShaderResource<ASVGFVarianceComputeShader>();
    variance_resources->ubo().BindResource(variance_uniform_buffer_);
    variance_resources->historyMomentsTexture().BindResource(history_moments_texture_[0]->GetDefaultView(rhi_));
    variance_resources->featureNormalRoughnessTexture().BindResource(
        feature_normal_roughness_texture_->GetDefaultView(rhi_));
    variance_resources->featureDepthTexture().BindResource(feature_depth_texture_->GetDefaultView(rhi_));
    variance_resources->outVarianceImage().BindResource(variance_texture_->GetDefaultView(rhi_));

    for (auto &atrous_pipeline_state : atrous_pipeline_states_)
    {
        auto *atrous_resources = atrous_pipeline_state->GetShaderResource<ASVGFAtrousComputeShader>();
        atrous_resources->ubo().BindResource(atrous_uniform_buffer_);
        atrous_resources->inputColorTexture().BindResource(history_color_texture_[0]->GetDefaultView(rhi_));
        atrous_resources->featureNormalRoughnessTexture().BindResource(
            feature_normal_roughness_texture_->GetDefaultView(rhi_));
        atrous_resources->featureAlbedoMetallicTexture().BindResource(
            feature_albedo_metallic_texture_->GetDefaultView(rhi_));
        atrous_resources->featureDepthTexture().BindResource(feature_depth_texture_->GetDefaultView(rhi_));
        atrous_resources->featurePrimitiveIdTexture().BindResource(feature_primitive_id_texture_->GetDefaultView(rhi_));
        atrous_resources->varianceTexture().BindResource(variance_texture_->GetDefaultView(rhi_));
        atrous_resources->outFilteredImage().BindResource(atrous_ping_pong_texture_[0]->GetDefaultView(rhi_));
    }

    auto *debug_resources = debug_pipeline_state_->GetShaderResource<ASVGFDebugVisualizeComputeShader>();
    debug_resources->ubo().BindResource(debug_uniform_buffer_);
    debug_resources->noisyTexture().BindResource(noisy_texture_->GetDefaultView(rhi_));
    debug_resources->featureNormalRoughnessTexture().BindResource(
        feature_normal_roughness_texture_->GetDefaultView(rhi_));
    debug_resources->featureAlbedoMetallicTexture().BindResource(
        feature_albedo_metallic_texture_->GetDefaultView(rhi_));
    debug_resources->featureDepthTexture().BindResource(feature_depth_texture_->GetDefaultView(rhi_));
    debug_resources->featurePrimitiveIdTexture().BindResource(feature_primitive_id_texture_->GetDefaultView(rhi_));
    debug_resources->reprojectionMaskTexture().BindResource(reprojection_mask_texture_->GetDefaultView(rhi_));
    debug_resources->historyMomentsTexture().BindResource(history_moments_texture_[0]->GetDefaultView(rhi_));
    debug_resources->historyColorTexture().BindResource(history_color_texture_[0]->GetDefaultView(rhi_));
    debug_resources->varianceTexture().BindResource(variance_texture_->GetDefaultView(rhi_));
    debug_resources->outputImage().BindResource(debug_texture_->GetDefaultView(rhi_));
    debug_resources->reprojectionDebugTexture().BindResource(reprojection_debug_texture_->GetDefaultView(rhi_));

    reprojection_compute_pass_ = rhi_->CreateComputePass("ASVGFReprojectionPass", false);
    variance_compute_pass_ = rhi_->CreateComputePass("ASVGFVariancePass", false);
    atrous_compute_pass_ = rhi_->CreateComputePass("ASVGFAtrousPass", false);
    debug_compute_pass_ = rhi_->CreateComputePass("ASVGFDebugPass", false);

    history_clear_pending_ = true;
}

bool ASVGF::HandleConfigStateChange(const RenderConfig &render_config)
{
    bool state_changed = render_config.asvgf != last_enabled_ || render_config.asvgf_debug_view != last_debug_view_ ||
                         render_config.asvgf_test_stage != last_test_stage_;

    if (!state_changed)
    {
        return false;
    }

    history_clear_pending_ = true;
    last_enabled_ = render_config.asvgf;
    last_debug_view_ = render_config.asvgf_debug_view;
    last_test_stage_ = render_config.asvgf_test_stage;
    return true;
}

void ASVGF::MarkHistoryClearPending()
{
    history_clear_pending_ = true;
}

ASVGF::RayTraceOutputTextures ASVGF::GetRayTraceOutputTextures(const RenderConfig &render_config) const
{
    if (render_config.asvgf && HasDenoiserTextures())
    {
        return {
            .noisy_texture = noisy_texture_,
            .feature_normal_roughness_texture = feature_normal_roughness_texture_,
            .feature_albedo_metallic_texture = feature_albedo_metallic_texture_,
            .feature_depth_texture = feature_depth_texture_,
            .feature_primitive_id_texture = feature_primitive_id_texture_,
        };
    }

    return fallback_ray_trace_output_textures_;
}

void ASVGF::TransitionRayTraceOutputTexturesForWrite(const RenderConfig &render_config) const
{
    const auto outputs = GetRayTraceOutputTextures(render_config);
    if (!outputs.IsValid())
    {
        return;
    }

    outputs.noisy_texture->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                       .after_stage = RHIPipelineStage::Top,
                                       .before_stage = RHIPipelineStage::ComputeShader});
    outputs.feature_normal_roughness_texture->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                          .after_stage = RHIPipelineStage::Top,
                                                          .before_stage = RHIPipelineStage::ComputeShader});
    outputs.feature_albedo_metallic_texture->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                         .after_stage = RHIPipelineStage::Top,
                                                         .before_stage = RHIPipelineStage::ComputeShader});
    outputs.feature_depth_texture->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                               .after_stage = RHIPipelineStage::Top,
                                               .before_stage = RHIPipelineStage::ComputeShader});
    outputs.feature_primitive_id_texture->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                      .after_stage = RHIPipelineStage::Top,
                                                      .before_stage = RHIPipelineStage::ComputeShader});
}

void ASVGF::RunPasses(const RenderConfig &render_config, uint32_t current_frame_spp)
{
    if (!render_config.asvgf || !HasPassResources())
    {
        return;
    }

    if (history_clear_pending_)
    {
        ResetHistoryResources();
        history_clear_pending_ = false;
    }

    const bool freeze_history = render_config.asvgf_freeze_history;
    const bool run_reprojection_pass =
        render_config.asvgf_test_stage != RenderConfig::ASVGFTestStage::raytrace && !freeze_history;
    const bool enable_temporal_accumulation =
        render_config.asvgf_test_stage != RenderConfig::ASVGFTestStage::reprojection;
    const bool run_variance_pass = render_config.asvgf_debug_view == RenderConfig::ASVGFDebugView::variance ||
                                   render_config.asvgf_test_stage == RenderConfig::ASVGFTestStage::variance ||
                                   render_config.asvgf_test_stage == RenderConfig::ASVGFTestStage::atrous_iter ||
                                   render_config.asvgf_test_stage == RenderConfig::ASVGFTestStage::off;
    const bool run_atrous_pass = render_config.asvgf_atrous_iterations > 0 &&
                                 (render_config.asvgf_test_stage == RenderConfig::ASVGFTestStage::atrous_iter ||
                                  render_config.asvgf_test_stage == RenderConfig::ASVGFTestStage::off);
    const bool use_debug_display = UseDebugDisplay(render_config);
    uint32_t read_history_index = history_index_;
    uint32_t write_history_index = (history_index_ + 1u) % 2u;
    uint32_t display_history_index = read_history_index;
    uint32_t effective_history_cap =
        std::max(std::max(render_config.asvgf_history_cap, 1u), render_config.max_sample_per_pixel);

    auto *camera = scene_render_proxy_->GetCamera();
    const auto focus_plane = camera->GetFocusPlane();
    const auto posture = camera->GetPosture();

    if (run_reprojection_pass)
    {
        auto to_vec4 = [](const Vector3 &value, float w) { return Vector4(value.x(), value.y(), value.z(), w); };

        ASVGFReprojectionComputeShader::UniformBufferData reprojection_ubo{
            .current_camera_position = to_vec4(posture.position, 1.f),
            .previous_camera_position =
                previous_camera_valid_ ? to_vec4(previous_camera_position_, 1.f) : to_vec4(posture.position, 1.f),
            .current_lower_left = to_vec4(focus_plane.lower_left, 0.f),
            .current_max_u = to_vec4(focus_plane.max_u, 0.f),
            .current_max_v = to_vec4(focus_plane.max_v, 0.f),
            .previous_lower_left =
                previous_camera_valid_ ? to_vec4(previous_lower_left_, 0.f) : to_vec4(focus_plane.lower_left, 0.f),
            .previous_max_u = previous_camera_valid_ ? to_vec4(previous_max_u_, 0.f) : to_vec4(focus_plane.max_u, 0.f),
            .previous_max_v = previous_camera_valid_ ? to_vec4(previous_max_v_, 0.f) : to_vec4(focus_plane.max_v, 0.f),
            .resolution_x = image_size_.x(),
            .resolution_y = image_size_.y(),
            .history_cap = effective_history_cap,
            .current_spp = current_frame_spp,
            .has_previous_camera = previous_camera_valid_ ? 1u : 0u,
            .enable_temporal_accumulation = enable_temporal_accumulation ? 1u : 0u,
        };
        reprojection_uniform_buffer_->Upload(rhi_, &reprojection_ubo);

        auto *reprojection_resources =
            reprojection_pipeline_state_->GetShaderResource<ASVGFReprojectionComputeShader>();
        reprojection_resources->prevHistoryColorTexture().BindResource(
            history_color_texture_[read_history_index]->GetDefaultView(rhi_));
        reprojection_resources->prevHistoryMomentsTexture().BindResource(
            history_moments_texture_[read_history_index]->GetDefaultView(rhi_));
        reprojection_resources->prevFeatureNormalRoughnessTexture().BindResource(
            history_feature_normal_roughness_texture_[read_history_index]->GetDefaultView(rhi_));
        reprojection_resources->prevFeatureDepthTexture().BindResource(
            history_feature_depth_texture_[read_history_index]->GetDefaultView(rhi_));
        reprojection_resources->prevFeaturePrimitiveIdTexture().BindResource(
            history_feature_primitive_id_texture_[read_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outHistoryColorImage().BindResource(
            history_color_texture_[write_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outHistoryMomentsImage().BindResource(
            history_moments_texture_[write_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outFeatureNormalRoughnessImage().BindResource(
            history_feature_normal_roughness_texture_[write_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outFeatureDepthImage().BindResource(
            history_feature_depth_texture_[write_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outFeaturePrimitiveIdImage().BindResource(
            history_feature_primitive_id_texture_[write_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outSceneColorImage().BindResource(scene_texture_->GetDefaultView(rhi_));

        noisy_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::ComputeShader});
        feature_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                       .after_stage = RHIPipelineStage::ComputeShader,
                                                       .before_stage = RHIPipelineStage::ComputeShader});
        feature_depth_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                            .after_stage = RHIPipelineStage::ComputeShader,
                                            .before_stage = RHIPipelineStage::ComputeShader});
        feature_primitive_id_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                   .after_stage = RHIPipelineStage::ComputeShader,
                                                   .before_stage = RHIPipelineStage::ComputeShader});

        history_color_texture_[read_history_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                .after_stage = RHIPipelineStage::ComputeShader,
                                                                .before_stage = RHIPipelineStage::ComputeShader});
        history_moments_texture_[read_history_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
        history_feature_normal_roughness_texture_[read_history_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});
        history_feature_depth_texture_[read_history_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});
        history_feature_primitive_id_texture_[read_history_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});

        history_color_texture_[write_history_index]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                 .after_stage = RHIPipelineStage::Top,
                                                                 .before_stage = RHIPipelineStage::ComputeShader});
        history_moments_texture_[write_history_index]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                   .after_stage = RHIPipelineStage::Top,
                                                                   .before_stage = RHIPipelineStage::ComputeShader});
        history_feature_normal_roughness_texture_[write_history_index]->Transition(
            {.target_layout = RHIImageLayout::StorageWrite,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        history_feature_depth_texture_[write_history_index]->Transition(
            {.target_layout = RHIImageLayout::StorageWrite,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        history_feature_primitive_id_texture_[write_history_index]->Transition(
            {.target_layout = RHIImageLayout::StorageWrite,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        reprojection_mask_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                .after_stage = RHIPipelineStage::Top,
                                                .before_stage = RHIPipelineStage::ComputeShader});
        reprojection_debug_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                 .after_stage = RHIPipelineStage::Top,
                                                 .before_stage = RHIPipelineStage::ComputeShader});
        scene_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::ComputeShader});

        rhi_->BeginComputePass(reprojection_compute_pass_);
        rhi_->DispatchCompute(reprojection_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
        rhi_->EndComputePass(reprojection_compute_pass_);

        history_moments_texture_[write_history_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                   .after_stage = RHIPipelineStage::ComputeShader,
                                                                   .before_stage = RHIPipelineStage::ComputeShader});
        reprojection_mask_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                .after_stage = RHIPipelineStage::ComputeShader,
                                                .before_stage = RHIPipelineStage::ComputeShader});
        reprojection_debug_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                 .after_stage = RHIPipelineStage::ComputeShader,
                                                 .before_stage = RHIPipelineStage::ComputeShader});
        scene_texture_->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = run_atrous_pass ? RHIPipelineStage::ComputeShader : RHIPipelineStage::PixelShader});

        history_index_ = write_history_index;
        display_history_index = write_history_index;
        previous_camera_position_ = posture.position;
        previous_lower_left_ = focus_plane.lower_left;
        previous_max_u_ = focus_plane.max_u;
        previous_max_v_ = focus_plane.max_v;
        previous_camera_valid_ = true;
    }

    if (run_variance_pass)
    {
        auto *variance_resources = variance_pipeline_state_->GetShaderResource<ASVGFVarianceComputeShader>();
        variance_resources->historyMomentsTexture().BindResource(
            history_moments_texture_[display_history_index]->GetDefaultView(rhi_));

        ASVGFVarianceComputeShader::UniformBufferData variance_ubo{
            .resolution_x = image_size_.x(),
            .resolution_y = image_size_.y(),
        };
        variance_uniform_buffer_->Upload(rhi_, &variance_ubo);

        history_moments_texture_[display_history_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                     .after_stage = RHIPipelineStage::ComputeShader,
                                                                     .before_stage = RHIPipelineStage::ComputeShader});
        feature_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                       .after_stage = RHIPipelineStage::ComputeShader,
                                                       .before_stage = RHIPipelineStage::ComputeShader});
        feature_depth_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                            .after_stage = RHIPipelineStage::ComputeShader,
                                            .before_stage = RHIPipelineStage::ComputeShader});
        variance_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                       .after_stage = RHIPipelineStage::Top,
                                       .before_stage = RHIPipelineStage::ComputeShader});

        rhi_->BeginComputePass(variance_compute_pass_);
        rhi_->DispatchCompute(variance_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
        rhi_->EndComputePass(variance_compute_pass_);

        variance_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                       .after_stage = RHIPipelineStage::ComputeShader,
                                       .before_stage = RHIPipelineStage::ComputeShader});
    }

    if (run_atrous_pass)
    {
        feature_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                       .after_stage = RHIPipelineStage::ComputeShader,
                                                       .before_stage = RHIPipelineStage::ComputeShader});
        feature_albedo_metallic_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                      .after_stage = RHIPipelineStage::ComputeShader,
                                                      .before_stage = RHIPipelineStage::ComputeShader});
        feature_depth_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                            .after_stage = RHIPipelineStage::ComputeShader,
                                            .before_stage = RHIPipelineStage::ComputeShader});
        feature_primitive_id_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                   .after_stage = RHIPipelineStage::ComputeShader,
                                                   .before_stage = RHIPipelineStage::ComputeShader});
        variance_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                       .after_stage = RHIPipelineStage::ComputeShader,
                                       .before_stage = RHIPipelineStage::ComputeShader});

        const uint32_t max_atrous_iterations = static_cast<uint32_t>(atrous_pipeline_states_.size());
        uint32_t iteration_count = render_config.asvgf_atrous_iterations;
        if (iteration_count > max_atrous_iterations)
        {
            iteration_count = max_atrous_iterations;
        }

        if (render_config.asvgf_test_stage == RenderConfig::ASVGFTestStage::off)
        {
            iteration_count = std::min(iteration_count, 2u);

            const uint32_t accumulated_spp = camera->GetCumulatedSampleCount();
            if (accumulated_spp >= 512u)
            {
                iteration_count = 0;
            }
            else if (accumulated_spp >= 128u)
            {
                iteration_count = std::min(iteration_count, 1u);
            }
        }

        RHIResourceRef<RHIImage> input_texture = history_color_texture_[display_history_index];
        for (uint32_t iteration = 0; iteration < iteration_count; ++iteration)
        {
            const bool is_last_iteration = (iteration + 1u) == iteration_count;
            auto output_texture = is_last_iteration
                                      ? scene_texture_
                                      : atrous_ping_pong_texture_[iteration % atrous_ping_pong_texture_.size()];
            auto atrous_pipeline_state = atrous_pipeline_states_[iteration];
            auto *atrous_resources = atrous_pipeline_state->GetShaderResource<ASVGFAtrousComputeShader>();

            ASVGFAtrousComputeShader::UniformBufferData atrous_ubo{
                .resolution_x = image_size_.x(),
                .resolution_y = image_size_.y(),
                .step_width = 1u << (iteration / 2u),
                .iteration_index = iteration,
            };
            atrous_uniform_buffer_->Upload(rhi_, &atrous_ubo);

            atrous_resources->inputColorTexture().BindResource(input_texture->GetDefaultView(rhi_));
            atrous_resources->outFilteredImage().BindResource(output_texture->GetDefaultView(rhi_));

            input_texture->Transition({.target_layout = RHIImageLayout::Read,
                                       .after_stage = RHIPipelineStage::ComputeShader,
                                       .before_stage = RHIPipelineStage::ComputeShader});
            output_texture->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                        .after_stage = RHIPipelineStage::Top,
                                        .before_stage = RHIPipelineStage::ComputeShader});

            rhi_->BeginComputePass(atrous_compute_pass_);
            rhi_->DispatchCompute(atrous_pipeline_state, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
            rhi_->EndComputePass(atrous_compute_pass_);

            if (is_last_iteration)
            {
                output_texture->Transition({.target_layout = RHIImageLayout::Read,
                                            .after_stage = RHIPipelineStage::ComputeShader,
                                            .before_stage = use_debug_display ? RHIPipelineStage::ComputeShader
                                                                              : RHIPipelineStage::PixelShader});
            }
            else
            {
                output_texture->Transition({.target_layout = RHIImageLayout::Read,
                                            .after_stage = RHIPipelineStage::ComputeShader,
                                            .before_stage = RHIPipelineStage::ComputeShader});
                input_texture = output_texture;
            }
        }
    }

    if (!use_debug_display)
    {
        return;
    }

    auto *debug_resources = debug_pipeline_state_->GetShaderResource<ASVGFDebugVisualizeComputeShader>();
    debug_resources->historyMomentsTexture().BindResource(
        history_moments_texture_[display_history_index]->GetDefaultView(rhi_));
    debug_resources->historyColorTexture().BindResource(scene_texture_->GetDefaultView(rhi_));

    ASVGFDebugVisualizeComputeShader::UniformBufferData debug_ubo{
        .stage = static_cast<uint32_t>(render_config.asvgf_test_stage),
        .view = static_cast<uint32_t>(render_config.asvgf_debug_view),
        .history_index = display_history_index,
        .frame_index = static_cast<uint32_t>(rhi_->GetRenderedFrameCount()),
        .resolution_x = image_size_.x(),
        .resolution_y = image_size_.y(),
        .freeze_history = render_config.asvgf_freeze_history ? 1u : 0u,
        .force_clear_history = render_config.asvgf_force_clear_history ? 1u : 0u,
        .history_cap = effective_history_cap,
    };
    debug_uniform_buffer_->Upload(rhi_, &debug_ubo);

    noisy_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});
    feature_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                   .after_stage = RHIPipelineStage::ComputeShader,
                                                   .before_stage = RHIPipelineStage::ComputeShader});
    feature_albedo_metallic_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                  .before_stage = RHIPipelineStage::ComputeShader});
    feature_depth_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                        .after_stage = RHIPipelineStage::ComputeShader,
                                        .before_stage = RHIPipelineStage::ComputeShader});
    feature_primitive_id_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                               .after_stage = RHIPipelineStage::ComputeShader,
                                               .before_stage = RHIPipelineStage::ComputeShader});
    reprojection_mask_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                            .after_stage = RHIPipelineStage::ComputeShader,
                                            .before_stage = RHIPipelineStage::ComputeShader});
    reprojection_debug_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                             .after_stage = RHIPipelineStage::ComputeShader,
                                             .before_stage = RHIPipelineStage::ComputeShader});
    variance_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                   .after_stage = RHIPipelineStage::ComputeShader,
                                   .before_stage = RHIPipelineStage::ComputeShader});
    scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});
    history_moments_texture_[display_history_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                 .after_stage = RHIPipelineStage::ComputeShader,
                                                                 .before_stage = RHIPipelineStage::ComputeShader});
    history_color_texture_[display_history_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                               .after_stage = RHIPipelineStage::ComputeShader,
                                                               .before_stage = RHIPipelineStage::ComputeShader});

    debug_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                .after_stage = RHIPipelineStage::Top,
                                .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(debug_compute_pass_);
    rhi_->DispatchCompute(debug_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(debug_compute_pass_);

    debug_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::PixelShader});
}

void ASVGF::UpdateFrameData(const RenderConfig &render_config, SceneRenderProxy *scene_render_proxy)
{
    if (debug_tone_mapping_pass_)
    {
        debug_tone_mapping_pass_->UpdateFrameData(render_config, scene_render_proxy);
    }
}

bool ASVGF::UseDebugDisplay(const RenderConfig &render_config) const
{
    return render_config.asvgf && debug_tone_mapping_pass_ &&
           render_config.asvgf_debug_view != RenderConfig::ASVGFDebugView::none;
}

ToneMappingPass *ASVGF::GetDebugToneMappingPass() const
{
    return debug_tone_mapping_pass_.get();
}

bool ASVGF::IsInitialized() const
{
    return HasPipelineResources() && HasDenoiserTextures();
}

bool ASVGF::HasPipelineResources() const
{
    return reprojection_pipeline_state_ && variance_pipeline_state_ && atrous_pipeline_states_[0] &&
           debug_pipeline_state_ && debug_tone_mapping_pass_;
}

bool ASVGF::HasPassResources() const
{
    return HasPipelineResources() && reprojection_compute_pass_ && variance_compute_pass_ && atrous_compute_pass_ &&
           debug_compute_pass_ && reprojection_uniform_buffer_ && variance_uniform_buffer_ && atrous_uniform_buffer_ &&
           debug_uniform_buffer_ && noisy_texture_ && feature_normal_roughness_texture_ &&
           feature_albedo_metallic_texture_ && feature_depth_texture_ && feature_primitive_id_texture_ &&
           reprojection_mask_texture_ && reprojection_debug_texture_ && variance_texture_ &&
           history_feature_normal_roughness_texture_[0] && history_feature_normal_roughness_texture_[1] &&
           history_feature_depth_texture_[0] && history_feature_depth_texture_[1] &&
           history_feature_primitive_id_texture_[0] && history_feature_primitive_id_texture_[1] &&
           history_color_texture_[0] && history_color_texture_[1] && history_moments_texture_[0] &&
           history_moments_texture_[1] && atrous_ping_pong_texture_[0] && atrous_ping_pong_texture_[1] &&
           scene_texture_;
}

bool ASVGF::HasDenoiserTextures() const
{
    return noisy_texture_ && feature_normal_roughness_texture_ && feature_albedo_metallic_texture_ &&
           feature_depth_texture_ && feature_primitive_id_texture_;
}

void ASVGF::ResetHistoryResources()
{
    history_index_ = 0;
    previous_camera_position_ = Vector3::Zero();
    previous_lower_left_ = Vector3::Zero();
    previous_max_u_ = Vector3::Zero();
    previous_max_v_ = Vector3::Zero();
    previous_camera_valid_ = false;
}
} // namespace sparkle
