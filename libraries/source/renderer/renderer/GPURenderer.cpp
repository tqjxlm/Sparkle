#include "renderer/renderer/GPURenderer.h"

#include "core/Profiler.h"
#include "renderer/BindlessManager.h"
#include "renderer/pass/ClearTexturePass.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/pass/ToneMappingPass.h"
#include "renderer/pass/UiPass.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"
#include "rhi/RHIRayTracing.h"

namespace sparkle
{
class RayTracingComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(RayTracingComputeShader, RHIShaderStage::Compute, "shaders/ray_trace/ray_trace.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(tlas, RHIShaderResourceReflection::ResourceType::AccelerationStructure)
    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(imageData, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(materialIdBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(materialBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(skyMap, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(skyMapSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(materialTextureSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(noisyImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(featureNormalRoughnessImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(featureAlbedoMetallicImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(featureDepthImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(featurePrimitiveIdImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)

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
        uint32_t asvgf_enabled;
    };
};

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
    USE_SHADER_RESOURCE(outputImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)

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
    };
};

GPURenderer::GPURenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                         SceneRenderProxy *scene_render_proxy)
    : Renderer(render_config, rhi_context, scene_render_proxy),
      spp_logger_(1.f, false, [this](float) { MeasurePerformance(); })
{
    ASSERT_EQUAL(render_config.pipeline, RenderConfig::Pipeline::gpu);

    ASSERT(rhi_->SupportsHardwareRayTracing());

    last_asvgf_enabled_ = render_config_.asvgf;
    last_asvgf_debug_view_ = render_config_.asvgf_debug_view;
    last_asvgf_test_stage_ = render_config_.asvgf_test_stage;
}

void GPURenderer::InitRenderResources()
{
    scene_render_proxy_->InitRenderResources(rhi_, render_config_);

    scene_texture_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = PixelFormat::RGBAFloat,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = image_size_.x(),
            .height = image_size_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV | RHIImage::ImageUsage::ColorAttachment,
            .memory_properties = RHIMemoryProperty::DeviceLocal,
            .mip_levels = 1,
            .msaa_samples = 1,
        },
        "GPUPipelineColorBuffer");

    scene_rt_ = rhi_->CreateRenderTarget({}, scene_texture_, nullptr, "GPUPipelineColorRT");

    tone_mapping_output_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = PixelFormat::B8G8R8A8_SRGB,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = image_size_.x(),
            .height = image_size_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::ColorAttachment |
                      RHIImage::ImageUsage::TransferSrc,
            .msaa_samples = 1,
        },
        "ToneMappingBuffer");

    tone_mapping_rt_ = rhi_->CreateRenderTarget({}, tone_mapping_output_, nullptr, "ToneMappingRT");

    InitSceneRenderResources();

    tone_mapping_pass_ = PipelinePass::Create<ToneMappingPass>(render_config_, rhi_, scene_texture_, tone_mapping_rt_);

    if (render_config_.asvgf)
    {
        InitASVGFRenderResources();
    }

    clear_pass_ = PipelinePass::Create<ClearTexturePass>(render_config_, rhi_, Vector4::Zero(),
                                                         RHIImageLayout::StorageWrite, scene_rt_);

    screen_quad_pass_ = PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, tone_mapping_output_,
                                                             rhi_->GetBackBufferRenderTarget());

    ui_pass_ = PipelinePass::Create<UiPass>(render_config_, rhi_, tone_mapping_rt_);

    performance_history_.resize(rhi_->GetMaxFramesInFlight());

    running_time_per_spp_ =
        1000.f / render_config_.target_framerate / static_cast<float>(render_config_.sample_per_pixel);

    compute_pass_ = rhi_->CreateComputePass("GPURendererComputePass", true);
}

void GPURenderer::Render()
{
    PROFILE_SCOPE("GPURenderer::Render");

    auto *camera = scene_render_proxy_->GetCamera();

    if (render_config_.asvgf && !asvgf_noisy_texture_)
    {
        InitASVGFRenderResources();
    }

    if (camera->NeedClear())
    {
        clear_pass_->Render();
        camera->ClearPixels();
        dispatched_sample_count_ = 0;
        asvgf_history_clear_pending_ = true;
    }

    if (render_config_.asvgf_force_clear_history)
    {
        asvgf_history_clear_pending_ = true;
    }

    // base pass: render to texture
    {
        if (render_config_.asvgf && asvgf_noisy_texture_ && asvgf_feature_normal_roughness_texture_ &&
            asvgf_feature_albedo_metallic_texture_ && asvgf_feature_depth_texture_ && asvgf_feature_primitive_id_texture_)
        {
            asvgf_noisy_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                              .after_stage = RHIPipelineStage::Top,
                                              .before_stage = RHIPipelineStage::ComputeShader});
            asvgf_feature_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                 .after_stage = RHIPipelineStage::Top,
                                                                 .before_stage = RHIPipelineStage::ComputeShader});
            asvgf_feature_albedo_metallic_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                .after_stage = RHIPipelineStage::Top,
                                                                .before_stage = RHIPipelineStage::ComputeShader});
            asvgf_feature_depth_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                      .after_stage = RHIPipelineStage::Top,
                                                      .before_stage = RHIPipelineStage::ComputeShader});
            asvgf_feature_primitive_id_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                             .after_stage = RHIPipelineStage::Top,
                                                             .before_stage = RHIPipelineStage::ComputeShader});
        }
        else if (asvgf_fallback_noisy_texture_ && asvgf_fallback_normal_roughness_texture_ &&
                 asvgf_fallback_albedo_metallic_texture_ && asvgf_fallback_depth_texture_ &&
                 asvgf_fallback_primitive_id_texture_)
        {
            asvgf_fallback_noisy_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                       .after_stage = RHIPipelineStage::Top,
                                                       .before_stage = RHIPipelineStage::ComputeShader});
            asvgf_fallback_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                  .after_stage = RHIPipelineStage::Top,
                                                                  .before_stage = RHIPipelineStage::ComputeShader});
            asvgf_fallback_albedo_metallic_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                                 .after_stage = RHIPipelineStage::Top,
                                                                 .before_stage = RHIPipelineStage::ComputeShader});
            asvgf_fallback_depth_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                       .after_stage = RHIPipelineStage::Top,
                                                       .before_stage = RHIPipelineStage::ComputeShader});
            asvgf_fallback_primitive_id_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                              .after_stage = RHIPipelineStage::Top,
                                                              .before_stage = RHIPipelineStage::ComputeShader});
        }

        scene_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                    .after_stage = RHIPipelineStage::Top,
                                    .before_stage = RHIPipelineStage::ComputeShader});

        rhi_->BeginComputePass(compute_pass_);

        rhi_->DispatchCompute(pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});

        rhi_->EndComputePass(compute_pass_);

        scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::PixelShader});
    }

    if (render_config_.asvgf)
    {
        RunASVGFPasses();
    }

    // screen space passes (post processing)
    {
        auto *tone_mapping_pass = tone_mapping_pass_.get();
        if (UseASVGFDebugDisplay())
        {
            tone_mapping_pass = asvgf_debug_tone_mapping_pass_.get();
        }

        tone_mapping_pass->Render();

        bool has_readback = ReadbackFinalOutputIfRequested(tone_mapping_rt_, false, RHIPipelineStage::ColorOutput);

        if (render_config_.render_ui)
        {
            if (has_readback)
            {
                tone_mapping_output_->Transition({.target_layout = RHIImageLayout::ColorOutput,
                                                  .after_stage = RHIPipelineStage::Transfer,
                                                  .before_stage = RHIPipelineStage::ColorOutput});
            }

            ui_pass_->Render();
        }

        has_readback = ReadbackFinalOutputIfRequested(tone_mapping_rt_, true, RHIPipelineStage::ColorOutput);

        tone_mapping_output_->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = has_readback ? RHIPipelineStage::Transfer : RHIPipelineStage::ColorOutput,
             .before_stage = RHIPipelineStage::PixelShader});
    }

    // screen pass: render texture on a screen quad
    screen_quad_pass_->Render();
}

GPURenderer::~GPURenderer() = default;

bool GPURenderer::IsReadyForAutoScreenshot() const
{
    return Renderer::IsReadyForAutoScreenshot() &&
           scene_render_proxy_->GetCamera()->GetCumulatedSampleCount() >= render_config_.max_sample_per_pixel;
}

void GPURenderer::Update()
{
    PROFILE_SCOPE("GPURenderer::Update");

    auto *camera = scene_render_proxy_->GetCamera();

    bool asvgf_state_changed = render_config_.asvgf != last_asvgf_enabled_ ||
                               render_config_.asvgf_debug_view != last_asvgf_debug_view_ ||
                               render_config_.asvgf_test_stage != last_asvgf_test_stage_;
    if (asvgf_state_changed)
    {
        camera->MarkPixelDirty();
        asvgf_history_clear_pending_ = true;

        last_asvgf_enabled_ = render_config_.asvgf;
        last_asvgf_debug_view_ = render_config_.asvgf_debug_view;
        last_asvgf_test_stage_ = render_config_.asvgf_test_stage;
    }

    if (render_config_.asvgf && !asvgf_debug_pipeline_state_)
    {
        InitASVGFRenderResources();
    }

    if (scene_render_proxy_->GetBindlessManager()->IsBufferDirty())
    {
        BindBindlessResources();
    }

    auto *sky_light = scene_render_proxy_->GetSkyLight();
    if (sky_light != bound_sky_proxy_)
    {
        bound_sky_proxy_ = sky_light;
        auto *cs_resources = pipeline_state_->GetShaderResource<RayTracingComputeShader>();

        if ((sky_light != nullptr) && sky_light->GetSkyMap())
        {
            auto sky_map = sky_light->GetSkyMap();

            cs_resources->skyMap().BindResource(sky_map->GetDefaultView(rhi_));
            cs_resources->skyMapSampler().BindResource(sky_map->GetSampler());
        }
        else
        {
            auto dummy_texture = rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
                .format = PixelFormat::RGBAFloat16,
                .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                            .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                            .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
                .usages = RHIImage::ImageUsage::Texture,
                .type = RHIImage::ImageType::Image2DCube,
            });
            cs_resources->skyMap().BindResource(dummy_texture->GetDefaultView(rhi_));
            cs_resources->skyMapSampler().BindResource(dummy_texture->GetSampler());
        }
    }

    bool need_rebuild_tlas = false;
    std::unordered_set<uint32_t> primitives_to_update;
    for (const auto &[type, primitive, from, to] : scene_render_proxy_->GetPrimitiveChangeList())
    {
        switch (type)
        {
        case SceneRenderProxy::PrimitiveChangeType::New:
        case SceneRenderProxy::PrimitiveChangeType::Move: {
            if (primitive->IsMesh() && primitive->GetPrimitiveIndex() != UINT_MAX)
            {
                const auto *mesh = primitive->As<MeshRenderProxy>();
                const auto &blas = mesh->GetAccelerationStructure();

                tlas_->SetBLAS(blas.get(), primitive->GetPrimitiveIndex());
            }
            else
            {
                tlas_->SetBLAS(nullptr, to);
            }

            need_rebuild_tlas = true;
            break;
        }
        case SceneRenderProxy::PrimitiveChangeType::Remove:
            tlas_->SetBLAS(nullptr, from);

            need_rebuild_tlas = true;
            break;
        case SceneRenderProxy::PrimitiveChangeType::Update:
            primitives_to_update.insert(to);
            break;
        default:
            UnImplemented(type);
            break;
        }
    }

    if (need_rebuild_tlas)
    {
        // structural change, rebuild TLAS
        tlas_->Build();

        auto *cs_resources = pipeline_state_->GetShaderResource<RayTracingComputeShader>();
        cs_resources->tlas().BindResource(tlas_, true);
    }
    else if (!primitives_to_update.empty())
    {
        // non-structural change, update TLAS
        tlas_->Update(primitives_to_update);
    }

    // Use a per-frame seed that advances every dispatch so fresh samples are generated
    // even after cumulated_sample_count is capped. Stays identical to GetCumulatedSampleCount()
    // before the cap, preserving determinism for functional tests.
    auto time_seed = dispatched_sample_count_;

    uint32_t spp;

    if (render_config_.use_dynamic_spp)
    {
        auto frame_index = rhi_->GetFrameIndex();

        auto gpu_time = compute_pass_->GetExecutionTime(frame_index);

        bool has_valid_history = gpu_time > 0;
        if (has_valid_history)
        {
            float average_time_per_spp = gpu_time / static_cast<float>(performance_history_[frame_index].spp);
            running_time_per_spp_ = utilities::Lerp(running_time_per_spp_, average_time_per_spp, 0.5f);

            float target_frame_time = 1000.f / render_config_.target_framerate;
            float last_frame_time = rhi_->GetFrameStats(frame_index).elapsed_time_ms;
            ASSERT(last_frame_time > 0.f);

            float time_left = target_frame_time - last_frame_time + gpu_time;
            float time_budget = time_left * render_config_.gpu_time_budget_ratio;

            auto optimal_spp = static_cast<uint32_t>(time_budget / running_time_per_spp_);

            spp = utilities::Clamp(optimal_spp, 1u, render_config_.max_sample_per_pixel);
        }
        else
        {
            spp = render_config_.sample_per_pixel;
        }

        performance_history_[frame_index].spp = spp;
    }
    else
    {
        spp = render_config_.sample_per_pixel;
    }

    auto camera_ubo = camera->GetUniformBufferData(render_config_);
    camera_ubo.mode = GetRayTraceDebugMode();

    RayTracingComputeShader::UniformBufferData ubo{
        .camera = camera_ubo,
        .time_seed = time_seed,
        .total_sample_count = camera->GetCumulatedSampleCount(),
        .spp = spp,
        .enable_nee = render_config_.enable_nee ? 1u : 0,
        .asvgf_enabled = render_config_.asvgf ? 1u : 0u,
    };

    dispatched_sample_count_ += spp;
    camera->AccumulateSample(spp);

    if (sky_light)
    {
        ubo.sky_light = sky_light->GetRenderData();
    }
    auto *dir_light = scene_render_proxy_->GetDirectionalLight();
    if (dir_light)
    {
        ubo.dir_light = dir_light->GetRenderData();
    }
    uniform_buffer_->Upload(rhi_, &ubo);

    screen_quad_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    if (ui_pass_)
    {
        ui_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

    tone_mapping_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    if (asvgf_debug_tone_mapping_pass_)
    {
        asvgf_debug_tone_mapping_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

    last_second_total_spp_ += spp;

    spp_logger_.Tick();
}

void GPURenderer::MeasurePerformance()
{
    static uint64_t last_frame_index = 0;

    auto frame_index = rhi_->GetRenderedFrameCount();

    auto frame_count = static_cast<float>(frame_index - last_frame_index);

    last_frame_index = frame_index;

    auto average_spp = static_cast<float>(last_second_total_spp_) / frame_count;

    Logger::LogToScreen("SPP", fmt::format("SPP: {: .1f}", average_spp));

    last_second_total_spp_ = 0;
}

void GPURenderer::InitSceneRenderResources()
{
    tlas_ = rhi_->CreateTLAS("TLAS");
    uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(RayTracingComputeShader::UniformBufferData),
                                          .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                          .mem_properties = RHIMemoryProperty::None,
                                          .is_dynamic = true},
                                         "GPURendererUniformBuffer");

    compute_shader_ = rhi_->CreateShader<RayTracingComputeShader>();

    pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "GPURendererPineline");
    pipeline_state_->SetShader<RHIShaderStage::Compute>(compute_shader_);

    pipeline_state_->Compile();

    auto *cs_resources = pipeline_state_->GetShaderResource<RayTracingComputeShader>();
    cs_resources->ubo().BindResource(uniform_buffer_);
    cs_resources->imageData().BindResource(scene_texture_->GetDefaultView(rhi_));
    cs_resources->tlas().BindResource(tlas_);

    auto create_asvgf_fallback_texture = [this](PixelFormat format, const std::string &name) {
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

    asvgf_fallback_noisy_texture_ = create_asvgf_fallback_texture(PixelFormat::RGBAFloat, "ASVGFFallbackNoisy");
    asvgf_fallback_normal_roughness_texture_ =
        create_asvgf_fallback_texture(PixelFormat::RGBAFloat16, "ASVGFFallbackNormalRoughness");
    asvgf_fallback_albedo_metallic_texture_ =
        create_asvgf_fallback_texture(PixelFormat::RGBAFloat16, "ASVGFFallbackAlbedoMetallic");
    asvgf_fallback_depth_texture_ = create_asvgf_fallback_texture(PixelFormat::R32_FLOAT, "ASVGFFallbackDepth");
    asvgf_fallback_primitive_id_texture_ =
        create_asvgf_fallback_texture(PixelFormat::R32_UINT, "ASVGFFallbackPrimitiveId");

    cs_resources->noisyImage().BindResource(asvgf_fallback_noisy_texture_->GetDefaultView(rhi_));
    cs_resources->featureNormalRoughnessImage().BindResource(asvgf_fallback_normal_roughness_texture_->GetDefaultView(
        rhi_));
    cs_resources->featureAlbedoMetallicImage().BindResource(asvgf_fallback_albedo_metallic_texture_->GetDefaultView(
        rhi_));
    cs_resources->featureDepthImage().BindResource(asvgf_fallback_depth_texture_->GetDefaultView(rhi_));
    cs_resources->featurePrimitiveIdImage().BindResource(asvgf_fallback_primitive_id_texture_->GetDefaultView(rhi_));

    auto dummy_texture_2d = rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
        .format = PixelFormat::R8G8B8A8_SRGB,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .usages = RHIImage::ImageUsage::Texture,
    });

    auto dummy_texture_cube = rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
        .format = PixelFormat::RGBAFloat16,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .usages = RHIImage::ImageUsage::Texture,
        .type = RHIImage::ImageType::Image2DCube,
    });

    cs_resources->skyMap().BindResource(dummy_texture_cube->GetDefaultView(rhi_));
    cs_resources->skyMapSampler().BindResource(dummy_texture_cube->GetSampler());

    cs_resources->materialTextureSampler().BindResource(dummy_texture_2d->GetSampler());

    BindBindlessResources();
}

void GPURenderer::InitASVGFRenderResources()
{
    if (asvgf_debug_pipeline_state_)
    {
        return;
    }

    auto create_asvgf_texture = [this](PixelFormat format, const std::string &name) {
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

    asvgf_noisy_texture_ = create_asvgf_texture(PixelFormat::RGBAFloat, "ASVGFNoisyTexture");
    asvgf_feature_normal_roughness_texture_ =
        create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFFeatureNormalRoughness");
    asvgf_feature_albedo_metallic_texture_ =
        create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFFeatureAlbedoMetallic");
    asvgf_feature_depth_texture_ = create_asvgf_texture(PixelFormat::R32_FLOAT, "ASVGFFeatureDepth");
    asvgf_feature_primitive_id_texture_ = create_asvgf_texture(PixelFormat::R32_UINT, "ASVGFFeaturePrimitiveId");
    asvgf_history_color_texture_[0] = create_asvgf_texture(PixelFormat::RGBAFloat, "ASVGFHistoryColor0");
    asvgf_history_color_texture_[1] = create_asvgf_texture(PixelFormat::RGBAFloat, "ASVGFHistoryColor1");
    asvgf_history_moments_texture_[0] = create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFHistoryMoments0");
    asvgf_history_moments_texture_[1] = create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFHistoryMoments1");
    asvgf_variance_texture_ = create_asvgf_texture(PixelFormat::R32_FLOAT, "ASVGFVariance");
    asvgf_atrous_ping_pong_texture_[0] = create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFAtrousPing");
    asvgf_atrous_ping_pong_texture_[1] = create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFAtrousPong");
    asvgf_debug_texture_ = create_asvgf_texture(PixelFormat::RGBAFloat, "ASVGFDebug");

    asvgf_debug_tone_mapping_pass_ =
        PipelinePass::Create<ToneMappingPass>(render_config_, rhi_, asvgf_debug_texture_, tone_mapping_rt_);

    asvgf_debug_uniform_buffer_ =
        rhi_->CreateBuffer({.size = sizeof(ASVGFDebugVisualizeComputeShader::UniformBufferData),
                            .usages = RHIBuffer::BufferUsage::UniformBuffer,
                            .mem_properties = RHIMemoryProperty::None,
                            .is_dynamic = true},
                           "ASVGFDebugUniformBuffer");
    asvgf_debug_shader_ = rhi_->CreateShader<ASVGFDebugVisualizeComputeShader>();
    asvgf_debug_pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ASVGFDebugPSO");
    asvgf_debug_pipeline_state_->SetShader<RHIShaderStage::Compute>(asvgf_debug_shader_);
    asvgf_debug_pipeline_state_->Compile();

    auto *raytrace_resources = pipeline_state_->GetShaderResource<RayTracingComputeShader>();
    raytrace_resources->noisyImage().BindResource(asvgf_noisy_texture_->GetDefaultView(rhi_));
    raytrace_resources->featureNormalRoughnessImage().BindResource(
        asvgf_feature_normal_roughness_texture_->GetDefaultView(rhi_));
    raytrace_resources->featureAlbedoMetallicImage().BindResource(
        asvgf_feature_albedo_metallic_texture_->GetDefaultView(rhi_));
    raytrace_resources->featureDepthImage().BindResource(asvgf_feature_depth_texture_->GetDefaultView(rhi_));
    raytrace_resources->featurePrimitiveIdImage().BindResource(asvgf_feature_primitive_id_texture_->GetDefaultView(rhi_));

    auto *cs_resources = asvgf_debug_pipeline_state_->GetShaderResource<ASVGFDebugVisualizeComputeShader>();
    cs_resources->ubo().BindResource(asvgf_debug_uniform_buffer_);
    cs_resources->noisyTexture().BindResource(asvgf_noisy_texture_->GetDefaultView(rhi_));
    cs_resources->featureNormalRoughnessTexture().BindResource(
        asvgf_feature_normal_roughness_texture_->GetDefaultView(rhi_));
    cs_resources->featureAlbedoMetallicTexture().BindResource(
        asvgf_feature_albedo_metallic_texture_->GetDefaultView(rhi_));
    cs_resources->featureDepthTexture().BindResource(asvgf_feature_depth_texture_->GetDefaultView(rhi_));
    cs_resources->featurePrimitiveIdTexture().BindResource(asvgf_feature_primitive_id_texture_->GetDefaultView(rhi_));
    cs_resources->outputImage().BindResource(asvgf_debug_texture_->GetDefaultView(rhi_));

    asvgf_debug_compute_pass_ = rhi_->CreateComputePass("ASVGFDebugPass", false);
    asvgf_history_clear_pending_ = true;
}

void GPURenderer::RunASVGFPasses()
{
    if (!render_config_.asvgf || !asvgf_debug_pipeline_state_ || !asvgf_noisy_texture_ ||
        !asvgf_feature_normal_roughness_texture_ || !asvgf_feature_albedo_metallic_texture_ ||
        !asvgf_feature_depth_texture_ || !asvgf_feature_primitive_id_texture_)
    {
        return;
    }

    if (asvgf_history_clear_pending_)
    {
        ResetASVGFHistoryResources();
        asvgf_history_clear_pending_ = false;
    }

    if (!UseASVGFDebugDisplay())
    {
        if (!render_config_.asvgf_freeze_history)
        {
            asvgf_history_index_ = (asvgf_history_index_ + 1) % 2;
        }

        return;
    }

    ASVGFDebugVisualizeComputeShader::UniformBufferData debug_ubo{
        .stage = static_cast<uint32_t>(render_config_.asvgf_test_stage),
        .view = static_cast<uint32_t>(render_config_.asvgf_debug_view),
        .history_index = asvgf_history_index_,
        .frame_index = static_cast<uint32_t>(rhi_->GetRenderedFrameCount()),
        .resolution_x = image_size_.x(),
        .resolution_y = image_size_.y(),
        .freeze_history = render_config_.asvgf_freeze_history ? 1u : 0u,
        .force_clear_history = render_config_.asvgf_force_clear_history ? 1u : 0u,
    };
    asvgf_debug_uniform_buffer_->Upload(rhi_, &debug_ubo);

    asvgf_noisy_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                      .after_stage = RHIPipelineStage::ComputeShader,
                                      .before_stage = RHIPipelineStage::ComputeShader});
    asvgf_feature_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                         .after_stage = RHIPipelineStage::ComputeShader,
                                                         .before_stage = RHIPipelineStage::ComputeShader});
    asvgf_feature_albedo_metallic_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                        .after_stage = RHIPipelineStage::ComputeShader,
                                                        .before_stage = RHIPipelineStage::ComputeShader});
    asvgf_feature_depth_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                              .after_stage = RHIPipelineStage::ComputeShader,
                                              .before_stage = RHIPipelineStage::ComputeShader});
    asvgf_feature_primitive_id_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                     .after_stage = RHIPipelineStage::ComputeShader,
                                                     .before_stage = RHIPipelineStage::ComputeShader});

    asvgf_debug_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                      .after_stage = RHIPipelineStage::Top,
                                      .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(asvgf_debug_compute_pass_);
    rhi_->DispatchCompute(asvgf_debug_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(asvgf_debug_compute_pass_);

    asvgf_debug_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                      .after_stage = RHIPipelineStage::ComputeShader,
                                      .before_stage = RHIPipelineStage::PixelShader});

    if (!render_config_.asvgf_freeze_history)
    {
        asvgf_history_index_ = (asvgf_history_index_ + 1) % 2;
    }
}

void GPURenderer::ResetASVGFHistoryResources()
{
    // Temporal resources are not consumed before S3, but reset state now to keep invalidation flow deterministic.
    asvgf_history_index_ = 0;
}

bool GPURenderer::UseASVGFDebugDisplay() const
{
    if (!render_config_.asvgf || !asvgf_debug_tone_mapping_pass_)
    {
        return false;
    }

    return render_config_.asvgf_debug_view != RenderConfig::ASVGFDebugView::none;
}

uint32_t GPURenderer::GetRayTraceDebugMode() const
{
    if (!render_config_.asvgf || render_config_.asvgf_debug_view == RenderConfig::ASVGFDebugView::none)
    {
        return static_cast<uint32_t>(render_config_.debug_mode);
    }

    return static_cast<uint32_t>(RenderConfig::DebugMode::Color);
}

void GPURenderer::BindBindlessResources()
{
    auto *cs_resources = pipeline_state_->GetShaderResource<RayTracingComputeShader>();

    const auto *bindless_manager = scene_render_proxy_->GetBindlessManager();

    cs_resources->materialIdBuffer().BindResource(bindless_manager->GetMaterialIdBuffer());
    cs_resources->materialBuffer().BindResource(bindless_manager->GetMaterialParameterBuffer());

    cs_resources->textures().BindResource(bindless_manager->GetBindlessBuffer(BindlessResourceType::Texture));
    cs_resources->indexBuffers().BindResource(bindless_manager->GetBindlessBuffer(BindlessResourceType::IndexBuffer));
    cs_resources->vertexBuffers().BindResource(bindless_manager->GetBindlessBuffer(BindlessResourceType::VertexBuffer));
    cs_resources->vertexAttributeBuffers().BindResource(
        bindless_manager->GetBindlessBuffer(BindlessResourceType::VertexAttributeBuffer));
}
} // namespace sparkle
