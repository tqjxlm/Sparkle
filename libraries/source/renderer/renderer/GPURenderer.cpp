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
    REGISTGER_SHADER(ASVGFAtrousComputeShader, RHIShaderStage::Compute, "shaders/ray_trace/asvgf_atrous.cs.slang", "main")

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
    }

    if (render_config_.asvgf_force_clear_history)
    {
        asvgf_history_clear_pending_ = true;
    }

    // base pass: render to texture
    {
        if (render_config_.asvgf && asvgf_noisy_texture_ && asvgf_feature_normal_roughness_texture_ &&
            asvgf_feature_albedo_metallic_texture_ && asvgf_feature_depth_texture_ &&
            asvgf_feature_primitive_id_texture_)
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

    if (render_config_.asvgf && (!asvgf_reprojection_pipeline_state_ || !asvgf_variance_pipeline_state_ ||
                                 !asvgf_atrous_pipeline_states_[0] || !asvgf_debug_pipeline_state_))
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
    bool scene_changed = false;
    std::unordered_set<uint32_t> primitives_to_update;
    for (const auto &[type, primitive, from, to] : scene_render_proxy_->GetPrimitiveChangeList())
    {
        scene_changed = true;
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

    if (scene_changed && render_config_.asvgf)
    {
        asvgf_history_clear_pending_ = true;
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
    cs_resources->featureNormalRoughnessImage().BindResource(
        asvgf_fallback_normal_roughness_texture_->GetDefaultView(rhi_));
    cs_resources->featureAlbedoMetallicImage().BindResource(
        asvgf_fallback_albedo_metallic_texture_->GetDefaultView(rhi_));
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
    if (asvgf_reprojection_pipeline_state_ && asvgf_variance_pipeline_state_ && asvgf_atrous_pipeline_states_[0] &&
        asvgf_debug_pipeline_state_)
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
    asvgf_history_feature_normal_roughness_texture_[0] =
        create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFHistoryFeatureNormalRoughness0");
    asvgf_history_feature_normal_roughness_texture_[1] =
        create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFHistoryFeatureNormalRoughness1");
    asvgf_history_feature_depth_texture_[0] = create_asvgf_texture(PixelFormat::R32_FLOAT, "ASVGFHistoryDepth0");
    asvgf_history_feature_depth_texture_[1] = create_asvgf_texture(PixelFormat::R32_FLOAT, "ASVGFHistoryDepth1");
    asvgf_history_feature_primitive_id_texture_[0] =
        create_asvgf_texture(PixelFormat::R32_UINT, "ASVGFHistoryPrimitiveId0");
    asvgf_history_feature_primitive_id_texture_[1] =
        create_asvgf_texture(PixelFormat::R32_UINT, "ASVGFHistoryPrimitiveId1");
    asvgf_history_color_texture_[0] = create_asvgf_texture(PixelFormat::RGBAFloat, "ASVGFHistoryColor0");
    asvgf_history_color_texture_[1] = create_asvgf_texture(PixelFormat::RGBAFloat, "ASVGFHistoryColor1");
    asvgf_history_moments_texture_[0] = create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFHistoryMoments0");
    asvgf_history_moments_texture_[1] = create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFHistoryMoments1");
    asvgf_variance_texture_ = create_asvgf_texture(PixelFormat::R32_FLOAT, "ASVGFVariance");
    asvgf_reprojection_mask_texture_ = create_asvgf_texture(PixelFormat::R32_FLOAT, "ASVGFReprojectionMask");
    asvgf_reprojection_debug_texture_ = create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFReprojectionDebug");
    asvgf_atrous_ping_pong_texture_[0] = create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFAtrousPing");
    asvgf_atrous_ping_pong_texture_[1] = create_asvgf_texture(PixelFormat::RGBAFloat16, "ASVGFAtrousPong");
    asvgf_debug_texture_ = create_asvgf_texture(PixelFormat::RGBAFloat, "ASVGFDebug");

    asvgf_debug_tone_mapping_pass_ =
        PipelinePass::Create<ToneMappingPass>(render_config_, rhi_, asvgf_debug_texture_, tone_mapping_rt_);

    asvgf_reprojection_uniform_buffer_ =
        rhi_->CreateBuffer({.size = sizeof(ASVGFReprojectionComputeShader::UniformBufferData),
                            .usages = RHIBuffer::BufferUsage::UniformBuffer,
                            .mem_properties = RHIMemoryProperty::None,
                            .is_dynamic = true},
                           "ASVGFReprojectionUniformBuffer");
    asvgf_reprojection_shader_ = rhi_->CreateShader<ASVGFReprojectionComputeShader>();
    asvgf_reprojection_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ASVGFReprojectionPSO");
    asvgf_reprojection_pipeline_state_->SetShader<RHIShaderStage::Compute>(asvgf_reprojection_shader_);
    asvgf_reprojection_pipeline_state_->Compile();

    asvgf_variance_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ASVGFVarianceComputeShader::UniformBufferData),
                                                         .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                         .mem_properties = RHIMemoryProperty::None,
                                                         .is_dynamic = true},
                                                        "ASVGFVarianceUniformBuffer");
    asvgf_variance_shader_ = rhi_->CreateShader<ASVGFVarianceComputeShader>();
    asvgf_variance_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ASVGFVariancePSO");
    asvgf_variance_pipeline_state_->SetShader<RHIShaderStage::Compute>(asvgf_variance_shader_);
    asvgf_variance_pipeline_state_->Compile();

    asvgf_atrous_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ASVGFAtrousComputeShader::UniformBufferData),
                                                        .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                        .mem_properties = RHIMemoryProperty::None,
                                                        .is_dynamic = true},
                                                       "ASVGFAtrousUniformBuffer");
    asvgf_atrous_shader_ = rhi_->CreateShader<ASVGFAtrousComputeShader>();
    for (uint32_t iteration = 0; iteration < asvgf_atrous_pipeline_states_.size(); ++iteration)
    {
        asvgf_atrous_pipeline_states_[iteration] =
            rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, fmt::format("ASVGFAtrousPSO{}", iteration));
        asvgf_atrous_pipeline_states_[iteration]->SetShader<RHIShaderStage::Compute>(asvgf_atrous_shader_);
        asvgf_atrous_pipeline_states_[iteration]->Compile();
    }

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
    raytrace_resources->featurePrimitiveIdImage().BindResource(
        asvgf_feature_primitive_id_texture_->GetDefaultView(rhi_));

    auto *reprojection_resources =
        asvgf_reprojection_pipeline_state_->GetShaderResource<ASVGFReprojectionComputeShader>();
    reprojection_resources->ubo().BindResource(asvgf_reprojection_uniform_buffer_);
    reprojection_resources->noisyTexture().BindResource(asvgf_noisy_texture_->GetDefaultView(rhi_));
    reprojection_resources->featureNormalRoughnessTexture().BindResource(
        asvgf_feature_normal_roughness_texture_->GetDefaultView(rhi_));
    reprojection_resources->featureDepthTexture().BindResource(asvgf_feature_depth_texture_->GetDefaultView(rhi_));
    reprojection_resources->featurePrimitiveIdTexture().BindResource(
        asvgf_feature_primitive_id_texture_->GetDefaultView(rhi_));
    reprojection_resources->outReprojectionMaskImage().BindResource(
        asvgf_reprojection_mask_texture_->GetDefaultView(rhi_));
    reprojection_resources->outReprojectionDebugImage().BindResource(
        asvgf_reprojection_debug_texture_->GetDefaultView(rhi_));
    reprojection_resources->outSceneColorImage().BindResource(scene_texture_->GetDefaultView(rhi_));

    auto *variance_resources = asvgf_variance_pipeline_state_->GetShaderResource<ASVGFVarianceComputeShader>();
    variance_resources->ubo().BindResource(asvgf_variance_uniform_buffer_);
    variance_resources->historyMomentsTexture().BindResource(asvgf_history_moments_texture_[0]->GetDefaultView(rhi_));
    variance_resources->featureNormalRoughnessTexture().BindResource(
        asvgf_feature_normal_roughness_texture_->GetDefaultView(rhi_));
    variance_resources->featureDepthTexture().BindResource(asvgf_feature_depth_texture_->GetDefaultView(rhi_));
    variance_resources->outVarianceImage().BindResource(asvgf_variance_texture_->GetDefaultView(rhi_));

    for (auto &atrous_pipeline_state : asvgf_atrous_pipeline_states_)
    {
        auto *atrous_resources = atrous_pipeline_state->GetShaderResource<ASVGFAtrousComputeShader>();
        atrous_resources->ubo().BindResource(asvgf_atrous_uniform_buffer_);
        atrous_resources->inputColorTexture().BindResource(asvgf_history_color_texture_[0]->GetDefaultView(rhi_));
        atrous_resources->featureNormalRoughnessTexture().BindResource(
            asvgf_feature_normal_roughness_texture_->GetDefaultView(rhi_));
        atrous_resources->featureAlbedoMetallicTexture().BindResource(
            asvgf_feature_albedo_metallic_texture_->GetDefaultView(rhi_));
        atrous_resources->featureDepthTexture().BindResource(asvgf_feature_depth_texture_->GetDefaultView(rhi_));
        atrous_resources->featurePrimitiveIdTexture().BindResource(
            asvgf_feature_primitive_id_texture_->GetDefaultView(rhi_));
        atrous_resources->varianceTexture().BindResource(asvgf_variance_texture_->GetDefaultView(rhi_));
        atrous_resources->outFilteredImage().BindResource(asvgf_atrous_ping_pong_texture_[0]->GetDefaultView(rhi_));
    }

    auto *debug_resources = asvgf_debug_pipeline_state_->GetShaderResource<ASVGFDebugVisualizeComputeShader>();
    debug_resources->ubo().BindResource(asvgf_debug_uniform_buffer_);
    debug_resources->noisyTexture().BindResource(asvgf_noisy_texture_->GetDefaultView(rhi_));
    debug_resources->featureNormalRoughnessTexture().BindResource(
        asvgf_feature_normal_roughness_texture_->GetDefaultView(rhi_));
    debug_resources->featureAlbedoMetallicTexture().BindResource(
        asvgf_feature_albedo_metallic_texture_->GetDefaultView(rhi_));
    debug_resources->featureDepthTexture().BindResource(asvgf_feature_depth_texture_->GetDefaultView(rhi_));
    debug_resources->featurePrimitiveIdTexture().BindResource(
        asvgf_feature_primitive_id_texture_->GetDefaultView(rhi_));
    debug_resources->reprojectionMaskTexture().BindResource(asvgf_reprojection_mask_texture_->GetDefaultView(rhi_));
    debug_resources->historyMomentsTexture().BindResource(asvgf_history_moments_texture_[0]->GetDefaultView(rhi_));
    debug_resources->historyColorTexture().BindResource(asvgf_history_color_texture_[0]->GetDefaultView(rhi_));
    debug_resources->varianceTexture().BindResource(asvgf_variance_texture_->GetDefaultView(rhi_));
    debug_resources->outputImage().BindResource(asvgf_debug_texture_->GetDefaultView(rhi_));
    debug_resources->reprojectionDebugTexture().BindResource(asvgf_reprojection_debug_texture_->GetDefaultView(rhi_));

    asvgf_reprojection_compute_pass_ = rhi_->CreateComputePass("ASVGFReprojectionPass", false);
    asvgf_variance_compute_pass_ = rhi_->CreateComputePass("ASVGFVariancePass", false);
    asvgf_atrous_compute_pass_ = rhi_->CreateComputePass("ASVGFAtrousPass", false);
    asvgf_debug_compute_pass_ = rhi_->CreateComputePass("ASVGFDebugPass", false);
    asvgf_history_clear_pending_ = true;
}

void GPURenderer::RunASVGFPasses()
{
    if (!render_config_.asvgf || !asvgf_reprojection_pipeline_state_ || !asvgf_variance_pipeline_state_ ||
        !asvgf_atrous_pipeline_states_[0] || !asvgf_debug_pipeline_state_ || !asvgf_reprojection_compute_pass_ ||
        !asvgf_variance_compute_pass_ || !asvgf_atrous_compute_pass_ || !asvgf_reprojection_uniform_buffer_ ||
        !asvgf_variance_uniform_buffer_ || !asvgf_atrous_uniform_buffer_ || !asvgf_noisy_texture_ ||
        !asvgf_feature_normal_roughness_texture_ || !asvgf_feature_albedo_metallic_texture_ ||
        !asvgf_feature_depth_texture_ || !asvgf_feature_primitive_id_texture_ || !asvgf_reprojection_mask_texture_ ||
        !asvgf_reprojection_debug_texture_ || !asvgf_variance_texture_ ||
        !asvgf_history_feature_normal_roughness_texture_[0] || !asvgf_history_feature_normal_roughness_texture_[1] ||
        !asvgf_history_feature_depth_texture_[0] || !asvgf_history_feature_depth_texture_[1] ||
        !asvgf_history_feature_primitive_id_texture_[0] || !asvgf_history_feature_primitive_id_texture_[1] ||
        !asvgf_history_color_texture_[0] || !asvgf_history_color_texture_[1] || !asvgf_history_moments_texture_[0] ||
        !asvgf_history_moments_texture_[1] || !asvgf_atrous_ping_pong_texture_[0] || !asvgf_atrous_ping_pong_texture_[1] ||
        !scene_texture_)
    {
        return;
    }

    if (asvgf_history_clear_pending_)
    {
        ResetASVGFHistoryResources();
        asvgf_history_clear_pending_ = false;
    }

    const bool freeze_history = render_config_.asvgf_freeze_history;
    const bool run_reprojection_pass =
        render_config_.asvgf_test_stage != RenderConfig::ASVGFTestStage::raytrace && !freeze_history;
    const bool enable_temporal_accumulation =
        render_config_.asvgf_test_stage != RenderConfig::ASVGFTestStage::reprojection;
    const bool run_variance_pass = render_config_.asvgf_debug_view == RenderConfig::ASVGFDebugView::variance ||
                                   render_config_.asvgf_test_stage == RenderConfig::ASVGFTestStage::variance ||
                                   render_config_.asvgf_test_stage == RenderConfig::ASVGFTestStage::atrous_iter ||
                                   render_config_.asvgf_test_stage == RenderConfig::ASVGFTestStage::off;
    const bool run_atrous_pass = render_config_.asvgf_atrous_iterations > 0 &&
                                 (render_config_.asvgf_test_stage == RenderConfig::ASVGFTestStage::atrous_iter ||
                                  render_config_.asvgf_test_stage == RenderConfig::ASVGFTestStage::off);
    const bool use_debug_display = UseASVGFDebugDisplay();
    uint32_t read_history_index = asvgf_history_index_;
    uint32_t write_history_index = (asvgf_history_index_ + 1u) % 2u;
    uint32_t display_history_index = read_history_index;

    auto *camera = scene_render_proxy_->GetCamera();
    const auto focus_plane = camera->GetFocusPlane();
    const auto posture = camera->GetPosture();

    if (run_reprojection_pass)
    {
        auto to_vec4 = [](const Vector3 &value, float w) { return Vector4(value.x(), value.y(), value.z(), w); };

        ASVGFReprojectionComputeShader::UniformBufferData reprojection_ubo{
            .current_camera_position = to_vec4(posture.position, 1.f),
            .previous_camera_position = asvgf_previous_camera_valid_ ? to_vec4(asvgf_previous_camera_position_, 1.f)
                                                                     : to_vec4(posture.position, 1.f),
            .current_lower_left = to_vec4(focus_plane.lower_left, 0.f),
            .current_max_u = to_vec4(focus_plane.max_u, 0.f),
            .current_max_v = to_vec4(focus_plane.max_v, 0.f),
            .previous_lower_left = asvgf_previous_camera_valid_ ? to_vec4(asvgf_previous_lower_left_, 0.f)
                                                                : to_vec4(focus_plane.lower_left, 0.f),
            .previous_max_u =
                asvgf_previous_camera_valid_ ? to_vec4(asvgf_previous_max_u_, 0.f) : to_vec4(focus_plane.max_u, 0.f),
            .previous_max_v =
                asvgf_previous_camera_valid_ ? to_vec4(asvgf_previous_max_v_, 0.f) : to_vec4(focus_plane.max_v, 0.f),
            .resolution_x = image_size_.x(),
            .resolution_y = image_size_.y(),
            .history_cap = render_config_.asvgf_history_cap,
            .has_previous_camera = asvgf_previous_camera_valid_ ? 1u : 0u,
            .enable_temporal_accumulation = enable_temporal_accumulation ? 1u : 0u,
        };
        asvgf_reprojection_uniform_buffer_->Upload(rhi_, &reprojection_ubo);

        auto *reprojection_resources =
            asvgf_reprojection_pipeline_state_->GetShaderResource<ASVGFReprojectionComputeShader>();
        reprojection_resources->prevHistoryColorTexture().BindResource(
            asvgf_history_color_texture_[read_history_index]->GetDefaultView(rhi_));
        reprojection_resources->prevHistoryMomentsTexture().BindResource(
            asvgf_history_moments_texture_[read_history_index]->GetDefaultView(rhi_));
        reprojection_resources->prevFeatureNormalRoughnessTexture().BindResource(
            asvgf_history_feature_normal_roughness_texture_[read_history_index]->GetDefaultView(rhi_));
        reprojection_resources->prevFeatureDepthTexture().BindResource(
            asvgf_history_feature_depth_texture_[read_history_index]->GetDefaultView(rhi_));
        reprojection_resources->prevFeaturePrimitiveIdTexture().BindResource(
            asvgf_history_feature_primitive_id_texture_[read_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outHistoryColorImage().BindResource(
            asvgf_history_color_texture_[write_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outHistoryMomentsImage().BindResource(
            asvgf_history_moments_texture_[write_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outFeatureNormalRoughnessImage().BindResource(
            asvgf_history_feature_normal_roughness_texture_[write_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outFeatureDepthImage().BindResource(
            asvgf_history_feature_depth_texture_[write_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outFeaturePrimitiveIdImage().BindResource(
            asvgf_history_feature_primitive_id_texture_[write_history_index]->GetDefaultView(rhi_));
        reprojection_resources->outSceneColorImage().BindResource(scene_texture_->GetDefaultView(rhi_));

        asvgf_noisy_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                          .after_stage = RHIPipelineStage::ComputeShader,
                                          .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_feature_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                             .after_stage = RHIPipelineStage::ComputeShader,
                                                             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_feature_depth_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                  .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_feature_primitive_id_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                         .after_stage = RHIPipelineStage::ComputeShader,
                                                         .before_stage = RHIPipelineStage::ComputeShader});

        asvgf_history_color_texture_[read_history_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                                      .after_stage = RHIPipelineStage::ComputeShader,
                                                                      .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_history_moments_texture_[read_history_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_history_feature_normal_roughness_texture_[read_history_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_history_feature_depth_texture_[read_history_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_history_feature_primitive_id_texture_[read_history_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});

        asvgf_history_color_texture_[write_history_index]->Transition(
            {.target_layout = RHIImageLayout::StorageWrite,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_history_moments_texture_[write_history_index]->Transition(
            {.target_layout = RHIImageLayout::StorageWrite,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_history_feature_normal_roughness_texture_[write_history_index]->Transition(
            {.target_layout = RHIImageLayout::StorageWrite,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_history_feature_depth_texture_[write_history_index]->Transition(
            {.target_layout = RHIImageLayout::StorageWrite,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_history_feature_primitive_id_texture_[write_history_index]->Transition(
            {.target_layout = RHIImageLayout::StorageWrite,
             .after_stage = RHIPipelineStage::Top,
             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_reprojection_mask_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                      .after_stage = RHIPipelineStage::Top,
                                                      .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_reprojection_debug_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                                       .after_stage = RHIPipelineStage::Top,
                                                       .before_stage = RHIPipelineStage::ComputeShader});
        scene_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::ComputeShader});

        rhi_->BeginComputePass(asvgf_reprojection_compute_pass_);
        rhi_->DispatchCompute(asvgf_reprojection_pipeline_state_, {image_size_.x(), image_size_.y(), 1u},
                              {16u, 16u, 1u});
        rhi_->EndComputePass(asvgf_reprojection_compute_pass_);

        asvgf_history_moments_texture_[write_history_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_reprojection_mask_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                      .after_stage = RHIPipelineStage::ComputeShader,
                                                      .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_reprojection_debug_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                       .after_stage = RHIPipelineStage::ComputeShader,
                                                       .before_stage = RHIPipelineStage::ComputeShader});
        scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = run_atrous_pass ? RHIPipelineStage::ComputeShader
                                                                    : RHIPipelineStage::PixelShader});

        asvgf_history_index_ = write_history_index;
        display_history_index = write_history_index;
        asvgf_previous_camera_position_ = posture.position;
        asvgf_previous_lower_left_ = focus_plane.lower_left;
        asvgf_previous_max_u_ = focus_plane.max_u;
        asvgf_previous_max_v_ = focus_plane.max_v;
        asvgf_previous_camera_valid_ = true;
    }

    if (run_variance_pass)
    {
        auto *variance_resources = asvgf_variance_pipeline_state_->GetShaderResource<ASVGFVarianceComputeShader>();
        variance_resources->historyMomentsTexture().BindResource(
            asvgf_history_moments_texture_[display_history_index]->GetDefaultView(rhi_));

        ASVGFVarianceComputeShader::UniformBufferData variance_ubo{
            .resolution_x = image_size_.x(),
            .resolution_y = image_size_.y(),
        };
        asvgf_variance_uniform_buffer_->Upload(rhi_, &variance_ubo);

        asvgf_history_moments_texture_[display_history_index]->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = RHIPipelineStage::ComputeShader,
             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_feature_normal_roughness_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                             .after_stage = RHIPipelineStage::ComputeShader,
                                                             .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_feature_depth_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                  .before_stage = RHIPipelineStage::ComputeShader});
        asvgf_variance_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                             .after_stage = RHIPipelineStage::Top,
                                             .before_stage = RHIPipelineStage::ComputeShader});

        rhi_->BeginComputePass(asvgf_variance_compute_pass_);
        rhi_->DispatchCompute(asvgf_variance_pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
        rhi_->EndComputePass(asvgf_variance_compute_pass_);

        asvgf_variance_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                             .after_stage = RHIPipelineStage::ComputeShader,
                                             .before_stage = RHIPipelineStage::ComputeShader});
    }

    if (run_atrous_pass)
    {
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
        asvgf_variance_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                             .after_stage = RHIPipelineStage::ComputeShader,
                                             .before_stage = RHIPipelineStage::ComputeShader});

        const uint32_t max_atrous_iterations = static_cast<uint32_t>(asvgf_atrous_pipeline_states_.size());
        uint32_t iteration_count = render_config_.asvgf_atrous_iterations;
        if (iteration_count > max_atrous_iterations)
        {
            iteration_count = max_atrous_iterations;
        }

        RHIResourceRef<RHIImage> input_texture = asvgf_history_color_texture_[display_history_index];
        for (uint32_t iteration = 0; iteration < iteration_count; ++iteration)
        {
            const bool is_last_iteration = (iteration + 1u) == iteration_count;
            auto output_texture =
                is_last_iteration ? scene_texture_ : asvgf_atrous_ping_pong_texture_[iteration % asvgf_atrous_ping_pong_texture_.size()];
            auto atrous_pipeline_state = asvgf_atrous_pipeline_states_[iteration];
            auto *atrous_resources = atrous_pipeline_state->GetShaderResource<ASVGFAtrousComputeShader>();

            ASVGFAtrousComputeShader::UniformBufferData atrous_ubo{
                .resolution_x = image_size_.x(),
                .resolution_y = image_size_.y(),
                .step_width = 1u << (iteration / 2u),
                .iteration_index = iteration,
            };
            asvgf_atrous_uniform_buffer_->Upload(rhi_, &atrous_ubo);

            atrous_resources->inputColorTexture().BindResource(input_texture->GetDefaultView(rhi_));
            atrous_resources->outFilteredImage().BindResource(output_texture->GetDefaultView(rhi_));

            input_texture->Transition({.target_layout = RHIImageLayout::Read,
                                       .after_stage = RHIPipelineStage::ComputeShader,
                                       .before_stage = RHIPipelineStage::ComputeShader});
            output_texture->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                        .after_stage = RHIPipelineStage::Top,
                                        .before_stage = RHIPipelineStage::ComputeShader});

            rhi_->BeginComputePass(asvgf_atrous_compute_pass_);
            rhi_->DispatchCompute(atrous_pipeline_state, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
            rhi_->EndComputePass(asvgf_atrous_compute_pass_);

            if (is_last_iteration)
            {
                output_texture->Transition(
                    {.target_layout = RHIImageLayout::Read,
                     .after_stage = RHIPipelineStage::ComputeShader,
                     .before_stage = use_debug_display ? RHIPipelineStage::ComputeShader : RHIPipelineStage::PixelShader});
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

    auto *debug_resources = asvgf_debug_pipeline_state_->GetShaderResource<ASVGFDebugVisualizeComputeShader>();
    debug_resources->historyMomentsTexture().BindResource(
        asvgf_history_moments_texture_[display_history_index]->GetDefaultView(rhi_));
    debug_resources->historyColorTexture().BindResource(scene_texture_->GetDefaultView(rhi_));

    ASVGFDebugVisualizeComputeShader::UniformBufferData debug_ubo{
        .stage = static_cast<uint32_t>(render_config_.asvgf_test_stage),
        .view = static_cast<uint32_t>(render_config_.asvgf_debug_view),
        .history_index = display_history_index,
        .frame_index = static_cast<uint32_t>(rhi_->GetRenderedFrameCount()),
        .resolution_x = image_size_.x(),
        .resolution_y = image_size_.y(),
        .freeze_history = render_config_.asvgf_freeze_history ? 1u : 0u,
        .force_clear_history = render_config_.asvgf_force_clear_history ? 1u : 0u,
        .history_cap = render_config_.asvgf_history_cap,
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
    asvgf_reprojection_mask_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                  .after_stage = RHIPipelineStage::ComputeShader,
                                                  .before_stage = RHIPipelineStage::ComputeShader});
    asvgf_reprojection_debug_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                                   .after_stage = RHIPipelineStage::ComputeShader,
                                                   .before_stage = RHIPipelineStage::ComputeShader});
    asvgf_variance_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                         .after_stage = RHIPipelineStage::ComputeShader,
                                         .before_stage = RHIPipelineStage::ComputeShader});
    scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});
    asvgf_history_moments_texture_[display_history_index]->Transition(
        {.target_layout = RHIImageLayout::Read,
         .after_stage = RHIPipelineStage::ComputeShader,
         .before_stage = RHIPipelineStage::ComputeShader});
    asvgf_history_color_texture_[display_history_index]->Transition({.target_layout = RHIImageLayout::Read,
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
}

void GPURenderer::ResetASVGFHistoryResources()
{
    asvgf_history_index_ = 0;
    asvgf_previous_camera_position_ = Vector3::Zero();
    asvgf_previous_lower_left_ = Vector3::Zero();
    asvgf_previous_max_u_ = Vector3::Zero();
    asvgf_previous_max_v_ = Vector3::Zero();
    asvgf_previous_camera_valid_ = false;
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
