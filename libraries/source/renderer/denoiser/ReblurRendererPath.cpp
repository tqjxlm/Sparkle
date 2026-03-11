#include "renderer/denoiser/ReblurRendererPath.h"

#include "core/Profiler.h"
#include "renderer/BindlessManager.h"
#include "renderer/denoiser/ReblurDenoiser.h"
#include "renderer/pass/ClearTexturePass.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"

#include <algorithm>

namespace sparkle
{
namespace
{
class SplitPathTracerShader : public RHIShaderInfo
{
    REGISTGER_SHADER(SplitPathTracerShader, RHIShaderStage::Compute, "shaders/ray_trace/ray_trace_split.cs.slang",
                     "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(tlas, RHIShaderResourceReflection::ResourceType::AccelerationStructure)
    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(imageData, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(materialIdBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(materialBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(skyMap, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(skyMapSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(materialTextureSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(diffuseOutput, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(specularOutput, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(normalRoughnessOutput, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(viewZOutput, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(motionVectorOutput, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(albedoOutput, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(ptAccumulation, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(sampleStatsOutput, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    USE_SHADER_RESOURCE_BINDLESS(textures, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE_BINDLESS(indexBuffers, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE_BINDLESS(vertexBuffers, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE_BINDLESS(vertexAttributeBuffers, RHIShaderResourceReflection::ResourceType::StorageBuffer)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        CameraRenderProxy::UniformBufferData camera;
        SkyRenderProxy::UniformBufferData sky_light = {};
        DirectionalLightRenderProxy::UniformBufferData dir_light = {};
        uint32_t time_seed;
        float output_limit = CameraRenderProxy::OutputLimit;
        uint32_t total_sample_count;
        uint32_t spp;
        uint32_t enable_nee;
        alignas(16) Vector4 hit_dist_params = {3.f, 0.1f, 20.f, -25.f};
        Mat4 view_matrix = Mat4::Identity();
        Mat4 world_to_clip = Mat4::Identity();
        Mat4 world_to_clip_prev = Mat4::Identity();
        uint32_t debug_mode = 0;
    };
};

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

[[nodiscard]] uint32_t GetCompositeMode(RenderConfig::ReblurDebugPass debug_pass)
{
    using DebugPass = RenderConfig::ReblurDebugPass;

    const bool is_ta_diagnostic =
        debug_pass == DebugPass::TADisocclusion || debug_pass == DebugPass::TAMotionVector ||
        debug_pass == DebugPass::TADepth || debug_pass == DebugPass::TAHistory ||
        debug_pass == DebugPass::TASpecHistory || debug_pass == DebugPass::TAMaterialId ||
        debug_pass == DebugPass::TAAccumSpeed || debug_pass == DebugPass::TASpecAccumSpeed ||
        debug_pass == DebugPass::TASpecMotionInputs || debug_pass == DebugPass::TASpecQualityDelta ||
        debug_pass == DebugPass::TASpecSurfaceInputs || debug_pass == DebugPass::TAMotionVectorFine;
    const bool is_ts_diagnostic = debug_pass == DebugPass::TSStabCount || debug_pass == DebugPass::TSSpecBlend ||
                                  debug_pass == DebugPass::TSSpecAntilagInputs ||
                                  debug_pass == DebugPass::TSSpecClampInputs;

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
    if (debug_pass == DebugPass::CompositeDiffuse)
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

ReblurRendererPath::ReblurRendererPath(const RenderConfig &render_config, RHIContext *rhi, Vector2UInt image_size,
                                       RHIImage *scene_texture, RHIImage *performance_sample_stats, RHITLAS *tlas)
    : render_config_(render_config), rhi_(rhi), image_size_(image_size), scene_texture_(scene_texture),
      performance_sample_stats_(performance_sample_stats), tlas_(tlas)
{
    CreateResources();
}

ReblurRendererPath::~ReblurRendererPath() = default;

void ReblurRendererPath::CreateResources()
{
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

    pt_accumulation_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = PixelFormat::RGBAFloat,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = image_size_.x(),
            .height = image_size_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV | RHIImage::ImageUsage::ColorAttachment,
            .memory_properties = RHIMemoryProperty::DeviceLocal,
        },
        "ReblurPTAccumulation");
    pt_accumulation_rt_ = rhi_->CreateRenderTarget({}, pt_accumulation_, nullptr, "ReblurPTAccumulationRT");
    pt_clear_pass_ = PipelinePass::Create<ClearTexturePass>(render_config_, rhi_, Vector4::Zero(),
                                                            RHIImageLayout::StorageWrite, pt_accumulation_rt_);

    diffuse_signal_ = make_aux_image(PixelFormat::RGBAFloat16, "ReblurDiffuseSignal");
    specular_signal_ = make_aux_image(PixelFormat::RGBAFloat16, "ReblurSpecularSignal");
    normal_roughness_ = make_aux_image(PixelFormat::RGBAFloat16, "ReblurNormalRoughness");
    view_z_ = make_aux_image(PixelFormat::R32_FLOAT, "ReblurViewZ");
    motion_vectors_ = make_aux_image(PixelFormat::RG16Float, "ReblurMotionVectors");
    albedo_metallic_ = make_aux_image(PixelFormat::RGBAFloat16, "ReblurAlbedoMetallic");

    split_pt_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(SplitPathTracerShader::UniformBufferData),
                                                   .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                   .mem_properties = RHIMemoryProperty::None,
                                                   .is_dynamic = true},
                                                  "SplitPTUniformBuffer");
    split_pt_shader_ = rhi_->CreateShader<SplitPathTracerShader>();
    split_pt_pipeline_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "SplitPathTracerPipeline");
    split_pt_pipeline_->SetShader<RHIShaderStage::Compute>(split_pt_shader_);
    split_pt_pipeline_->Compile();

    auto *split_resources = split_pt_pipeline_->GetShaderResource<SplitPathTracerShader>();
    split_resources->ubo().BindResource(split_pt_uniform_buffer_);
    split_resources->imageData().BindResource(scene_texture_->GetDefaultView(rhi_));
    split_resources->tlas().BindResource(tlas_);
    split_resources->diffuseOutput().BindResource(diffuse_signal_->GetDefaultView(rhi_));
    split_resources->specularOutput().BindResource(specular_signal_->GetDefaultView(rhi_));
    split_resources->normalRoughnessOutput().BindResource(normal_roughness_->GetDefaultView(rhi_));
    split_resources->viewZOutput().BindResource(view_z_->GetDefaultView(rhi_));
    split_resources->motionVectorOutput().BindResource(motion_vectors_->GetDefaultView(rhi_));
    split_resources->albedoOutput().BindResource(albedo_metallic_->GetDefaultView(rhi_));
    split_resources->ptAccumulation().BindResource(pt_accumulation_->GetDefaultView(rhi_));
    split_resources->sampleStatsOutput().BindResource(performance_sample_stats_->GetDefaultView(rhi_));

    auto dummy_texture_2d = rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
        .format = PixelFormat::R8G8B8A8_SRGB,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .usages = RHIImage::ImageUsage::Texture,
    });
    split_resources->materialTextureSampler().BindResource(dummy_texture_2d->GetSampler());
    BindSkyLight(nullptr);

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
    comp_resources->viewZ().BindResource(view_z_->GetDefaultView(rhi_));
    comp_resources->ptAccumulated().BindResource(pt_accumulation_->GetDefaultView(rhi_));

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

    compute_pass_ = rhi_->CreateComputePass("ReblurGPUComputePass", false);
    reblur_ = std::make_unique<ReblurDenoiser>(rhi_, image_size_.x(), image_size_.y());
    ResetFinalHistory();
}

void ReblurRendererPath::BindBindlessResources(const BindlessManager &bindless_manager)
{
    auto *resources = split_pt_pipeline_->GetShaderResource<SplitPathTracerShader>();
    resources->materialIdBuffer().BindResource(bindless_manager.GetMaterialIdBuffer());
    resources->materialBuffer().BindResource(bindless_manager.GetMaterialParameterBuffer());
    resources->textures().BindResource(bindless_manager.GetBindlessBuffer(BindlessResourceType::Texture));
    resources->indexBuffers().BindResource(bindless_manager.GetBindlessBuffer(BindlessResourceType::IndexBuffer));
    resources->vertexBuffers().BindResource(bindless_manager.GetBindlessBuffer(BindlessResourceType::VertexBuffer));
    resources->vertexAttributeBuffers().BindResource(
        bindless_manager.GetBindlessBuffer(BindlessResourceType::VertexAttributeBuffer));
}

void ReblurRendererPath::BindSkyLight(const SkyRenderProxy *sky_light)
{
    auto *resources = split_pt_pipeline_->GetShaderResource<SplitPathTracerShader>();
    if ((sky_light != nullptr) && sky_light->GetSkyMap())
    {
        auto sky_map = sky_light->GetSkyMap();
        resources->skyMap().BindResource(sky_map->GetDefaultView(rhi_));
        resources->skyMapSampler().BindResource(sky_map->GetSampler());
        return;
    }

    auto dummy_texture = rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
        .format = PixelFormat::RGBAFloat16,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .usages = RHIImage::ImageUsage::Texture,
        .type = RHIImage::ImageType::Image2DCube,
    });
    resources->skyMap().BindResource(dummy_texture->GetDefaultView(rhi_));
    resources->skyMapSampler().BindResource(dummy_texture->GetSampler());
}

void ReblurRendererPath::BindTlas(RHITLAS *tlas, bool force_rebind)
{
    tlas_ = tlas;
    split_pt_pipeline_->GetShaderResource<SplitPathTracerShader>()->tlas().BindResource(tlas_, force_rebind);
}

void ReblurRendererPath::ClearPathAccumulation()
{
    pt_clear_pass_->Render();
}

void ReblurRendererPath::UpdateFrameData(const CameraRenderProxy &camera, const SkyRenderProxy *sky_light,
                                         const DirectionalLightRenderProxy *dir_light,
                                         const ReblurFrameParameters &parameters)
{
    SplitPathTracerShader::UniformBufferData split_ubo{
        .camera = camera.GetUniformBufferData(render_config_),
        .time_seed = parameters.time_seed,
        .total_sample_count = parameters.total_sample_count,
        .spp = parameters.sample_per_pixel,
        .enable_nee = parameters.enable_nee ? 1u : 0u,
        .view_matrix = camera.GetViewMatrix(),
        .world_to_clip = camera.GetViewProjectionMatrix(),
        .world_to_clip_prev = camera.GetViewProjectionMatrixPrev(),
        .debug_mode = static_cast<uint32_t>(parameters.debug_mode),
    };
    if (sky_light != nullptr)
    {
        split_ubo.sky_light = sky_light->GetRenderData();
    }
    if (dir_light != nullptr)
    {
        split_ubo.dir_light = dir_light->GetRenderData();
    }
    split_pt_uniform_buffer_->Upload(rhi_, &split_ubo);

    const uint32_t composite_frame_index = render_config_.reblur_no_pt_blend ? 0u : parameters.total_sample_count;
    ReblurCompositeShader::UniformBufferData composite_ubo{
        .resolution = {image_size_.x(), image_size_.y()},
        .frame_index = composite_frame_index,
        .composite_mode = GetCompositeMode(render_config_.reblur_debug_pass),
    };
    composite_uniform_buffer_->Upload(rhi_, &composite_ubo);

    dispatched_sample_count_ = parameters.dispatched_sample_count;
}

void ReblurRendererPath::Render(const CameraRenderProxy &camera)
{
    PROFILE_SCOPE("ReblurRendererPath::Render");

    auto transition_aux_to_write = [](RHIImage *image) {
        image->Transition({.target_layout = RHIImageLayout::StorageWrite,
                           .after_stage = RHIPipelineStage::Top,
                           .before_stage = RHIPipelineStage::ComputeShader});
    };

    scene_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                .after_stage = RHIPipelineStage::Top,
                                .before_stage = RHIPipelineStage::ComputeShader});
    transition_aux_to_write(diffuse_signal_.get());
    transition_aux_to_write(specular_signal_.get());
    transition_aux_to_write(normal_roughness_.get());
    transition_aux_to_write(view_z_.get());
    transition_aux_to_write(motion_vectors_.get());
    transition_aux_to_write(albedo_metallic_.get());
    transition_aux_to_write(pt_accumulation_.get());
    transition_aux_to_write(performance_sample_stats_);

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(split_pt_pipeline_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);

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

    transition_aux_to_read(diffuse_signal_.get());
    transition_aux_to_read(specular_signal_.get());
    transition_aux_to_read(normal_roughness_.get());
    transition_aux_to_read(view_z_.get());
    transition_aux_to_read(motion_vectors_.get());
    transition_aux_to_read(albedo_metallic_.get());

    ReblurInputBuffers inputs{
        .diffuse_radiance_hit_dist = diffuse_signal_.get(),
        .specular_radiance_hit_dist = specular_signal_.get(),
        .normal_roughness = normal_roughness_.get(),
        .view_z = view_z_.get(),
        .motion_vectors = motion_vectors_.get(),
        .albedo_metallic = albedo_metallic_.get(),
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
    reblur_->Denoise(inputs, settings, matrices, dispatched_sample_count_, render_config_.reblur_debug_pass);

    using DebugPass = RenderConfig::ReblurDebugPass;
    RHIImage *composite_albedo = (render_config_.reblur_debug_pass == DebugPass::Full ||
                                  render_config_.reblur_debug_pass == DebugPass::CompositeDiffuse ||
                                  render_config_.reblur_debug_pass == DebugPass::StabilizedAlbedo)
                                     ? reblur_->GetCompositeAlbedoMetallic()
                                     : albedo_metallic_.get();

    auto *comp_resources = composite_pipeline_->GetShaderResource<ReblurCompositeShader>();
    comp_resources->denoisedDiffuse().BindResource(reblur_->GetDenoisedDiffuse()->GetDefaultView(rhi_));
    comp_resources->denoisedSpecular().BindResource(reblur_->GetDenoisedSpecular()->GetDefaultView(rhi_));
    comp_resources->albedoMetallic().BindResource(composite_albedo->GetDefaultView(rhi_));
    comp_resources->internalData().BindResource(reblur_->GetInternalData()->GetDefaultView(rhi_));

    reblur_->GetDenoisedDiffuse()->Transition({.target_layout = RHIImageLayout::Read,
                                               .after_stage = RHIPipelineStage::ComputeShader,
                                               .before_stage = RHIPipelineStage::ComputeShader});
    reblur_->GetDenoisedSpecular()->Transition({.target_layout = RHIImageLayout::Read,
                                                .after_stage = RHIPipelineStage::ComputeShader,
                                                .before_stage = RHIPipelineStage::ComputeShader});
    composite_albedo->Transition({.target_layout = RHIImageLayout::Read,
                                  .after_stage = RHIPipelineStage::ComputeShader,
                                  .before_stage = RHIPipelineStage::ComputeShader});
    pt_accumulation_->Transition({.target_layout = RHIImageLayout::Read,
                                  .after_stage = RHIPipelineStage::ComputeShader,
                                  .before_stage = RHIPipelineStage::ComputeShader});
    scene_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(composite_pipeline_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);

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

void ReblurRendererPath::Reset()
{
    ResetFinalHistory();
    reblur_->Reset();
}

void ReblurRendererPath::ResetFinalHistory()
{
    final_history_valid_ = false;
    final_history_ping_pong_ = 0;
}

bool ReblurRendererPath::ShouldStabilizeFinalHistory() const
{
    return render_config_.reblur_debug_pass == RenderConfig::ReblurDebugPass::Full &&
           !render_config_.reblur_no_pt_blend;
}

void ReblurRendererPath::StabilizeFinalHistory()
{
    uint32_t prev_idx = final_history_ping_pong_;
    uint32_t cur_idx = 1u - final_history_ping_pong_;

    auto *resources = final_history_pipeline_->GetShaderResource<ReblurFinalHistoryShader>();
    resources->inCurrentColor().BindResource(scene_texture_->GetDefaultView(rhi_));
    resources->inNormalRoughness().BindResource(normal_roughness_->GetDefaultView(rhi_));
    resources->inViewZ().BindResource(view_z_->GetDefaultView(rhi_));
    resources->inMotionVectors().BindResource(motion_vectors_->GetDefaultView(rhi_));
    resources->inInternalData().BindResource(reblur_->GetInternalData()->GetDefaultView(rhi_));
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
    normal_roughness_->Transition({.target_layout = RHIImageLayout::Read,
                                   .after_stage = RHIPipelineStage::Transfer,
                                   .before_stage = RHIPipelineStage::ComputeShader});
    view_z_->Transition({.target_layout = RHIImageLayout::Read,
                         .after_stage = RHIPipelineStage::Transfer,
                         .before_stage = RHIPipelineStage::ComputeShader});
    motion_vectors_->Transition({.target_layout = RHIImageLayout::Read,
                                 .after_stage = RHIPipelineStage::ComputeShader,
                                 .before_stage = RHIPipelineStage::ComputeShader});
    reblur_->GetInternalData()->Transition({.target_layout = RHIImageLayout::Read,
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

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(final_history_pipeline_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);

    view_z_->Transition({.target_layout = RHIImageLayout::TransferSrc,
                         .after_stage = RHIPipelineStage::ComputeShader,
                         .before_stage = RHIPipelineStage::Transfer});
    prev_final_view_z_->Transition(
        {.target_layout = RHIImageLayout::TransferDst,
         .after_stage = final_history_valid_ ? RHIPipelineStage::ComputeShader : RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::Transfer});
    view_z_->CopyToImage(prev_final_view_z_.get());

    normal_roughness_->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                   .after_stage = RHIPipelineStage::ComputeShader,
                                   .before_stage = RHIPipelineStage::Transfer});
    prev_final_normal_roughness_->Transition(
        {.target_layout = RHIImageLayout::TransferDst,
         .after_stage = final_history_valid_ ? RHIPipelineStage::ComputeShader : RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::Transfer});
    normal_roughness_->CopyToImage(prev_final_normal_roughness_.get());

    final_history_[cur_idx]->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                         .after_stage = RHIPipelineStage::ComputeShader,
                                         .before_stage = RHIPipelineStage::Transfer});
    scene_texture_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::Transfer});
    final_history_[cur_idx]->CopyToImage(scene_texture_);

    view_z_->Transition({.target_layout = RHIImageLayout::Read,
                         .after_stage = RHIPipelineStage::Transfer,
                         .before_stage = RHIPipelineStage::ComputeShader});
    normal_roughness_->Transition({.target_layout = RHIImageLayout::Read,
                                   .after_stage = RHIPipelineStage::Transfer,
                                   .before_stage = RHIPipelineStage::ComputeShader});
    prev_final_view_z_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::Transfer,
                                    .before_stage = RHIPipelineStage::ComputeShader});
    prev_final_normal_roughness_->Transition({.target_layout = RHIImageLayout::Read,
                                              .after_stage = RHIPipelineStage::Transfer,
                                              .before_stage = RHIPipelineStage::ComputeShader});
    final_history_[cur_idx]->Transition({.target_layout = RHIImageLayout::Read,
                                         .after_stage = RHIPipelineStage::Transfer,
                                         .before_stage = RHIPipelineStage::ComputeShader});
    scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::Transfer,
                                .before_stage = RHIPipelineStage::PixelShader});

    final_history_ping_pong_ = cur_idx;
    final_history_valid_ = true;
}
} // namespace sparkle
