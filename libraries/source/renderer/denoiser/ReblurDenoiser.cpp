#include "renderer/denoiser/ReblurDenoiser.h"

#include "core/Profiler.h"
#include "renderer/denoiser/ReblurDenoisingPipeline.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <utility>

namespace sparkle
{
namespace
{
class ReblurCompositeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurCompositeShader, RHIShaderStage::Compute, "shaders/ray_trace/reblur_composite.cs.slang",
                     "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(denoisedDiffuse, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(denoisedSpecular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(albedoMetallic, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outputImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(viewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(internalData, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(ptAccumulated, RHIShaderResourceReflection::ResourceType::Texture2D)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        Vector2UInt resolution;
        uint32_t frame_index;
        uint32_t composite_mode;
    };
};

class ReblurFinalHistoryShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurFinalHistoryShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/reblur_final_history.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(inCurrentColor, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inMotionVectors, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inInternalData, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevColor, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(linearSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(outColor, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        Vector2UInt resolution;
        float disocclusion_threshold;
        float denoising_range;
        uint32_t reset_history;
    };
};

[[maybe_unused]] constexpr auto *kReblurCompositeShaderGetStage = &ReblurCompositeShader::GetStage;
[[maybe_unused]] constexpr auto *kReblurCompositeShaderGetInfo = &ReblurCompositeShader::GetShaderInfo;
[[maybe_unused]] constexpr auto *kReblurFinalHistoryShaderGetStage = &ReblurFinalHistoryShader::GetStage;
[[maybe_unused]] constexpr auto *kReblurFinalHistoryShaderGetInfo = &ReblurFinalHistoryShader::GetShaderInfo;

[[nodiscard]] uint32_t GetCompositeMode(RenderConfig::ReblurDebugPass debug_pass)
{
    using DebugPass = RenderConfig::ReblurDebugPass;

    const bool is_ta_diagnostic =
        debug_pass == DebugPass::TADisocclusion || debug_pass == DebugPass::TAMotionVector ||
        debug_pass == DebugPass::TADepth || debug_pass == DebugPass::TAHistory ||
        debug_pass == DebugPass::TASpecHistory || debug_pass == DebugPass::TAMaterialId ||
        debug_pass == DebugPass::TAAccumSpeed || debug_pass == DebugPass::TASpecAccumSpeed ||
        debug_pass == DebugPass::TASpecMotionInputs || debug_pass == DebugPass::TASpecQualityDelta ||
        debug_pass == DebugPass::TASpecSurfaceInputs || debug_pass == DebugPass::TAMotionVectorFine ||
        debug_pass == DebugPass::TAPlaneDistance;
    const bool is_ts_diagnostic = debug_pass == DebugPass::TSStabCount || debug_pass == DebugPass::TSSpecBlend ||
                                  debug_pass == DebugPass::TSSpecAntilagInputs ||
                                  debug_pass == DebugPass::TSSpecClampInputs ||
                                  debug_pass == DebugPass::TSDiffClampInputs;

    if (debug_pass == DebugPass::StabilizedDiffuse)
    {
        return 1u;
    }
    if (debug_pass == DebugPass::TASpecHistory || debug_pass == DebugPass::TASpecAccumSpeed ||
        debug_pass == DebugPass::TASpecMotionInputs || debug_pass == DebugPass::TASpecQualityDelta ||
        debug_pass == DebugPass::TASpecSurfaceInputs || debug_pass == DebugPass::TemporalAccumSpecular ||
        debug_pass == DebugPass::HistoryFixSpecular || debug_pass == DebugPass::BlurSpecular ||
        debug_pass == DebugPass::PostBlurSpecular || debug_pass == DebugPass::StabilizedSpecular ||
        debug_pass == DebugPass::TSSpecBlend || debug_pass == DebugPass::TSSpecAntilagInputs ||
        debug_pass == DebugPass::TSSpecClampInputs)
    {
        return 5u;
    }
    if (is_ta_diagnostic || is_ts_diagnostic)
    {
        return 1u;
    }
    if (debug_pass == DebugPass::CompositeDiffuse || debug_pass == DebugPass::CompositeDiffuseRawAlbedo)
    {
        return 2u;
    }
    if (debug_pass == DebugPass::CompositeSpecular)
    {
        return 3u;
    }
    if (debug_pass == DebugPass::StabilizedAlbedo)
    {
        return 4u;
    }
    return 0u;
}
} // namespace

ReblurDenoiser::ReblurDenoiser(const RenderConfig &render_config, RHIContext *rhi, Vector2UInt image_size,
                               RHIImage *scene_texture, RHIImage *performance_sample_stats, RHITLAS *tlas)
    : render_config_(render_config), rhi_(rhi), image_size_(std::move(image_size)), scene_texture_(scene_texture)
{
    CreateResources(performance_sample_stats, tlas);
}

ReblurDenoiser::~ReblurDenoiser() = default;

void ReblurDenoiser::CreateResources(RHIImage *performance_sample_stats, RHITLAS *tlas)
{
    signal_generator_ = std::make_unique<ReblurSignalGenerator>(render_config_, rhi_, image_size_, scene_texture_,
                                                                performance_sample_stats, tlas);

    auto make_aux_image = [this](PixelFormat format, const std::string &name,
                                 RHIImage::ImageUsage extra_usages = RHIImage::ImageUsage::Undefined) {
        return rhi_->CreateImage(
            RHIImage::Attribute{
                .format = format,
                .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                            .filtering_method_min = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
                .width = image_size_.x(),
                .height = image_size_.y(),
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV |
                          RHIImage::ImageUsage::TransferSrc | extra_usages,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
            },
            name);
    };

    composite_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurCompositeShader::UniformBufferData),
                                                    .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                    .mem_properties = RHIMemoryProperty::None,
                                                    .is_dynamic = true},
                                                   "CompositeUniformBuffer");
    composite_shader_ = rhi_->CreateShader<ReblurCompositeShader>();
    composite_pipeline_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurCompositePipeline");
    composite_pipeline_->SetShader<RHIShaderStage::Compute>(composite_shader_);
    composite_pipeline_->Compile();

    auto *comp_resources = composite_pipeline_->GetShaderResource<ReblurCompositeShader>();
    comp_resources->ubo().BindResource(composite_uniform_buffer_);
    comp_resources->outputImage().BindResource(scene_texture_->GetDefaultView(rhi_));
    comp_resources->viewZ().BindResource(signal_generator_->GetViewZ()->GetDefaultView(rhi_));
    comp_resources->ptAccumulated().BindResource(signal_generator_->GetPathTracingAccumulation()->GetDefaultView(rhi_));

    final_history_[0] = make_aux_image(PixelFormat::RGBAFloat, "ReblurFinalHistory0");
    final_history_[1] = make_aux_image(PixelFormat::RGBAFloat, "ReblurFinalHistory1");
    prev_final_view_z_ =
        make_aux_image(PixelFormat::R32_FLOAT, "ReblurFinalPrevViewZ", RHIImage::ImageUsage::TransferDst);
    prev_final_normal_roughness_ =
        make_aux_image(PixelFormat::RGBAFloat16, "ReblurFinalPrevNormalRoughness", RHIImage::ImageUsage::TransferDst);

    final_history_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurFinalHistoryShader::UniformBufferData),
                                                        .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                        .mem_properties = RHIMemoryProperty::None,
                                                        .is_dynamic = true},
                                                       "ReblurFinalHistoryUniformBuffer");
    final_history_shader_ = rhi_->CreateShader<ReblurFinalHistoryShader>();
    final_history_pipeline_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurFinalHistoryPipeline");
    final_history_pipeline_->SetShader<RHIShaderStage::Compute>(final_history_shader_);
    final_history_pipeline_->Compile();
    final_history_pipeline_->GetShaderResource<ReblurFinalHistoryShader>()->ubo().BindResource(
        final_history_uniform_buffer_);

    resolve_pass_ = rhi_->CreateComputePass("ReblurResolveComputePass", false);
    denoising_pipeline_ = std::make_unique<ReblurDenoisingPipeline>(rhi_, image_size_.x(), image_size_.y());
    ResetFinalHistory();
}

void ReblurDenoiser::BindBindlessResources(const BindlessManager &bindless_manager)
{
    signal_generator_->BindBindlessResources(bindless_manager);
}

void ReblurDenoiser::BindSkyLight(const SkyRenderProxy *sky_light)
{
    signal_generator_->BindSkyLight(sky_light);
}

void ReblurDenoiser::BindTlas(RHITLAS *tlas, bool force_rebind)
{
    signal_generator_->BindTlas(tlas, force_rebind);
}

void ReblurDenoiser::ClearPathTracingAccumulation()
{
    signal_generator_->ClearPathTracingAccumulation();
}

void ReblurDenoiser::PrepareForPathTracing()
{
    signal_generator_->PrepareForPathTracing();
}

void ReblurDenoiser::UpdateFrameData(const CameraRenderProxy &camera, const SkyRenderProxy *sky_light,
                                     const DirectionalLightRenderProxy *dir_light,
                                     const ReblurPathTracingParameters &parameters)
{
    signal_generator_->UpdateFrameData(camera, sky_light, dir_light, parameters);

    const uint32_t composite_frame_index = render_config_.reblur_no_pt_blend ? 0u : parameters.total_sample_count;
    ReblurCompositeShader::UniformBufferData composite_ubo{
        .resolution = {image_size_.x(), image_size_.y()},
        .frame_index = composite_frame_index,
        .composite_mode = GetCompositeMode(render_config_.reblur_debug_pass),
    };
    composite_uniform_buffer_->Upload(rhi_, &composite_ubo);

    dispatched_sample_count_ = parameters.dispatched_sample_count;
}

const RHIResourceRef<RHIPipelineState> &ReblurDenoiser::GetPathTracingPipeline() const
{
    return signal_generator_->GetPathTracingPipeline();
}

void ReblurDenoiser::ResolveSceneTexture(const CameraRenderProxy &camera)
{
    PROFILE_SCOPE("ReblurDenoiser::ResolveSceneTexture");

    if (render_config_.reblur_debug_pass == RenderConfig::ReblurDebugPass::Passthrough)
    {
        ResetFinalHistory();
        scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::PixelShader});
        return;
    }

    auto transition_aux_to_read = [](RHIImage *image) {
        image->Transition({.target_layout = RHIImageLayout::Read,
                           .after_stage = RHIPipelineStage::ComputeShader,
                           .before_stage = RHIPipelineStage::ComputeShader});
    };

    transition_aux_to_read(signal_generator_->GetDiffuseSignal());
    transition_aux_to_read(signal_generator_->GetSpecularSignal());
    transition_aux_to_read(signal_generator_->GetNormalRoughness());
    transition_aux_to_read(signal_generator_->GetViewZ());
    transition_aux_to_read(signal_generator_->GetMotionVectors());
    transition_aux_to_read(signal_generator_->GetAlbedoMetallic());

    ReblurDenoiserInputs inputs{
        .diffuse_radiance_hit_dist = signal_generator_->GetDiffuseSignal(),
        .specular_radiance_hit_dist = signal_generator_->GetSpecularSignal(),
        .normal_roughness = signal_generator_->GetNormalRoughness(),
        .view_z = signal_generator_->GetViewZ(),
        .motion_vectors = signal_generator_->GetMotionVectors(),
        .albedo_metallic = signal_generator_->GetAlbedoMetallic(),
    };
    ReblurSettings settings;
    ReblurMatrices matrices;
    matrices.view_to_clip = camera.GetProjectionMatrix();
    matrices.view_to_world = camera.GetViewMatrix().inverse();
    matrices.world_to_clip_prev = camera.GetViewProjectionMatrixPrev();
    matrices.world_to_view_prev = camera.GetViewMatrixPrev();
    matrices.world_prev_to_world = Mat4::Identity();
    matrices.camera_delta = camera.GetPositionPrev() - camera.GetPosture().position;
    matrices.framerate_scale =
        std::max(33.333f / std::max(rhi_->GetFrameStats(rhi_->GetFrameIndex()).elapsed_time_ms, 1.0f), 1.0f);

    denoising_pipeline_->Denoise(inputs, settings, matrices, dispatched_sample_count_,
                                 render_config_.reblur_debug_pass);

    using DebugPass = RenderConfig::ReblurDebugPass;
    RHIImage *composite_albedo = (render_config_.reblur_debug_pass == DebugPass::Full ||
                                  render_config_.reblur_debug_pass == DebugPass::CompositeDiffuse ||
                                  render_config_.reblur_debug_pass == DebugPass::StabilizedAlbedo)
                                     ? denoising_pipeline_->GetStabilizedAlbedoMetallic()
                                     : signal_generator_->GetAlbedoMetallic();

    auto *comp_resources = composite_pipeline_->GetShaderResource<ReblurCompositeShader>();
    comp_resources->denoisedDiffuse().BindResource(denoising_pipeline_->GetDenoisedDiffuse()->GetDefaultView(rhi_));
    comp_resources->denoisedSpecular().BindResource(denoising_pipeline_->GetDenoisedSpecular()->GetDefaultView(rhi_));
    comp_resources->albedoMetallic().BindResource(composite_albedo->GetDefaultView(rhi_));
    comp_resources->internalData().BindResource(denoising_pipeline_->GetInternalData()->GetDefaultView(rhi_));

    denoising_pipeline_->GetDenoisedDiffuse()->Transition({.target_layout = RHIImageLayout::Read,
                                                           .after_stage = RHIPipelineStage::ComputeShader,
                                                           .before_stage = RHIPipelineStage::ComputeShader});
    denoising_pipeline_->GetDenoisedSpecular()->Transition({.target_layout = RHIImageLayout::Read,
                                                            .after_stage = RHIPipelineStage::ComputeShader,
                                                            .before_stage = RHIPipelineStage::ComputeShader});
    composite_albedo->Transition({.target_layout = RHIImageLayout::Read,
                                  .after_stage = RHIPipelineStage::ComputeShader,
                                  .before_stage = RHIPipelineStage::ComputeShader});
    signal_generator_->GetPathTracingAccumulation()->Transition({.target_layout = RHIImageLayout::Read,
                                                                 .after_stage = RHIPipelineStage::ComputeShader,
                                                                 .before_stage = RHIPipelineStage::ComputeShader});
    scene_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(resolve_pass_);
    rhi_->DispatchCompute(composite_pipeline_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(resolve_pass_);

    if (ShouldStabilizeFinalHistory())
    {
        StabilizeFinalHistory();
        return;
    }

    ResetFinalHistory();
    scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::PixelShader});
}

void ReblurDenoiser::Reset()
{
    ResetFinalHistory();
    denoising_pipeline_->Reset();
}

void ReblurDenoiser::ResetFinalHistory()
{
    final_history_valid_ = false;
    final_history_ping_pong_ = 0;
}

bool ReblurDenoiser::ShouldStabilizeFinalHistory() const
{
    return render_config_.reblur_debug_pass == RenderConfig::ReblurDebugPass::Full &&
           !render_config_.reblur_no_pt_blend;
}

void ReblurDenoiser::StabilizeFinalHistory()
{
    const uint32_t prev_idx = final_history_ping_pong_;
    const uint32_t cur_idx = 1u - final_history_ping_pong_;

    auto *resources = final_history_pipeline_->GetShaderResource<ReblurFinalHistoryShader>();
    resources->inCurrentColor().BindResource(scene_texture_->GetDefaultView(rhi_));
    resources->inNormalRoughness().BindResource(signal_generator_->GetNormalRoughness()->GetDefaultView(rhi_));
    resources->inViewZ().BindResource(signal_generator_->GetViewZ()->GetDefaultView(rhi_));
    resources->inMotionVectors().BindResource(signal_generator_->GetMotionVectors()->GetDefaultView(rhi_));
    resources->inInternalData().BindResource(denoising_pipeline_->GetInternalData()->GetDefaultView(rhi_));
    resources->prevColor().BindResource(final_history_[prev_idx]->GetDefaultView(rhi_));
    resources->prevViewZ().BindResource(prev_final_view_z_->GetDefaultView(rhi_));
    resources->prevNormalRoughness().BindResource(prev_final_normal_roughness_->GetDefaultView(rhi_));
    resources->linearSampler().BindResource(final_history_[prev_idx]->GetSampler());
    resources->outColor().BindResource(final_history_[cur_idx]->GetDefaultView(rhi_));

    ReblurFinalHistoryShader::UniformBufferData ubo{
        .resolution = {image_size_.x(), image_size_.y()},
        .disocclusion_threshold = 0.01f,
        .denoising_range = 1000.f,
        .reset_history = final_history_valid_ ? 0u : 1u,
    };
    final_history_uniform_buffer_->Upload(rhi_, &ubo);

    scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});
    signal_generator_->GetNormalRoughness()->Transition({.target_layout = RHIImageLayout::Read,
                                                         .after_stage = RHIPipelineStage::Transfer,
                                                         .before_stage = RHIPipelineStage::ComputeShader});
    signal_generator_->GetViewZ()->Transition({.target_layout = RHIImageLayout::Read,
                                               .after_stage = RHIPipelineStage::Transfer,
                                               .before_stage = RHIPipelineStage::ComputeShader});
    signal_generator_->GetMotionVectors()->Transition({.target_layout = RHIImageLayout::Read,
                                                       .after_stage = RHIPipelineStage::ComputeShader,
                                                       .before_stage = RHIPipelineStage::ComputeShader});
    denoising_pipeline_->GetInternalData()->Transition({.target_layout = RHIImageLayout::Read,
                                                        .after_stage = RHIPipelineStage::ComputeShader,
                                                        .before_stage = RHIPipelineStage::ComputeShader});
    final_history_[prev_idx]->Transition(
        {.target_layout = RHIImageLayout::Read,
         .after_stage = final_history_valid_ ? RHIPipelineStage::Transfer : RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::ComputeShader});
    prev_final_view_z_->Transition(
        {.target_layout = RHIImageLayout::Read,
         .after_stage = final_history_valid_ ? RHIPipelineStage::Transfer : RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::ComputeShader});
    prev_final_normal_roughness_->Transition(
        {.target_layout = RHIImageLayout::Read,
         .after_stage = final_history_valid_ ? RHIPipelineStage::Transfer : RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::ComputeShader});
    final_history_[cur_idx]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                         .after_stage = RHIPipelineStage::Top,
                                         .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(resolve_pass_);
    rhi_->DispatchCompute(final_history_pipeline_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(resolve_pass_);

    signal_generator_->GetViewZ()->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                               .after_stage = RHIPipelineStage::ComputeShader,
                                               .before_stage = RHIPipelineStage::Transfer});
    prev_final_view_z_->Transition(
        {.target_layout = RHIImageLayout::TransferDst,
         .after_stage = final_history_valid_ ? RHIPipelineStage::ComputeShader : RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::Transfer});
    signal_generator_->GetViewZ()->CopyToImage(prev_final_view_z_.get());

    signal_generator_->GetNormalRoughness()->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                                         .after_stage = RHIPipelineStage::ComputeShader,
                                                         .before_stage = RHIPipelineStage::Transfer});
    prev_final_normal_roughness_->Transition(
        {.target_layout = RHIImageLayout::TransferDst,
         .after_stage = final_history_valid_ ? RHIPipelineStage::ComputeShader : RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::Transfer});
    signal_generator_->GetNormalRoughness()->CopyToImage(prev_final_normal_roughness_.get());

    final_history_[cur_idx]->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                         .after_stage = RHIPipelineStage::ComputeShader,
                                         .before_stage = RHIPipelineStage::Transfer});
    scene_texture_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::Transfer});
    final_history_[cur_idx]->CopyToImage(scene_texture_);

    scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::Transfer,
                                .before_stage = RHIPipelineStage::PixelShader});

    final_history_ping_pong_ = cur_idx;
    final_history_valid_ = true;
}
} // namespace sparkle
