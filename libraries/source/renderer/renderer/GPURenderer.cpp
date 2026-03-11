#include "renderer/renderer/GPURenderer.h"

#include "core/Logger.h"
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
    USE_SHADER_RESOURCE(sampleStatsOutput, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(materialIdBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(materialBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(skyMap, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(skyMapSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(materialTextureSampler, RHIShaderResourceReflection::ResourceType::Sampler)

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
    };
};

GPURenderer::GPURenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                         SceneRenderProxy *scene_render_proxy)
    : Renderer(render_config, rhi_context, scene_render_proxy),
      performance_monitor_(render_config, rhi_context, image_size_),
      spp_logger_(1.f, false, [this](float) { MeasurePerformance(); })
{
    ASSERT_EQUAL(render_config.pipeline, RenderConfig::Pipeline::gpu);

    ASSERT(rhi_->SupportsHardwareRayTracing());
}

void GPURenderer::InitRenderResources()
{
    Log(Info, "GPURenderer initializing ({}x{})", image_size_.x(), image_size_.y());
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
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV |
                      RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::TransferDst,
            .memory_properties = RHIMemoryProperty::DeviceLocal,
            .mip_levels = 1,
            .msaa_samples = 1,
        },
        "GPUPipelineColorBuffer");

    scene_rt_ = rhi_->CreateRenderTarget({}, scene_texture_, nullptr, "GPUPipelineColorRT");

    performance_sample_stats_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = PixelFormat::RG32Float,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
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
        "GPUPerformanceSampleStats");

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

    clear_pass_ = PipelinePass::Create<ClearTexturePass>(render_config_, rhi_, Vector4::Zero(),
                                                         RHIImageLayout::StorageWrite, scene_rt_);

    screen_quad_pass_ = PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, tone_mapping_output_,
                                                             rhi_->GetBackBufferRenderTarget());

    if (!rhi_->IsHeadless())
    {
        ui_pass_ = PipelinePass::Create<UiPass>(render_config_, rhi_, tone_mapping_rt_);
    }

    performance_history_.resize(rhi_->GetMaxFramesInFlight());

    running_time_per_spp_ =
        1000.f / render_config_.target_framerate / static_cast<float>(render_config_.sample_per_pixel);

    compute_pass_ = rhi_->CreateComputePass("GPURendererComputePass", true);

    if (render_config_.use_reblur)
    {
        reblur_path_ = std::make_unique<ReblurRendererPath>(render_config_, rhi_, image_size_, scene_texture_.get(),
                                                            performance_sample_stats_.get(), tlas_.get());
        reblur_path_->BindBindlessResources(*scene_render_proxy_->GetBindlessManager());
        Log(Info, "GPURenderer: REBLUR denoiser enabled");
    }
    else
    {
        Log(Info, "GPURenderer: REBLUR denoiser disabled, using unified path");
    }
}

void GPURenderer::Render()
{
    PROFILE_SCOPE("GPURenderer::Render");

    auto *camera = scene_render_proxy_->GetCamera();

    if (camera->NeedClear())
    {
        clear_pass_->Render();
        if (reblur_path_)
        {
            reblur_path_->ClearPathAccumulation();
        }
        camera->ClearPixels();
        // NOTE: Do NOT reset reblur on camera motion. The vanilla accumulation buffer
        // must restart from scratch on camera movement, but the reblur denoiser
        // preserves temporal history across camera changes via motion-vector
        // reprojection. Calling Reset() would set reset_history=1, causing the
        // temporal accumulation shader to discard all converged history and output
        // a raw 1-spp sample — exactly the failure mode tested by
        // test_converged_history.py.
    }

    if (reblur_path_)
    {
        reblur_path_->Render(*camera);
    }
    else
    {
        // Original path: render to texture
        scene_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                    .after_stage = RHIPipelineStage::Top,
                                    .before_stage = RHIPipelineStage::ComputeShader});
        performance_sample_stats_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                               .after_stage = RHIPipelineStage::Top,
                                               .before_stage = RHIPipelineStage::ComputeShader});

        rhi_->BeginComputePass(compute_pass_);

        rhi_->DispatchCompute(pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});

        rhi_->EndComputePass(compute_pass_);

        scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::PixelShader});
    }

    performance_monitor_.Measure(performance_sample_stats_.get(), RHIPipelineStage::ComputeShader,
                                 dispatched_sample_count_, current_frame_spp_);
    performance_monitor_.UpdateOverlay();

    // screen space passes (post processing)
    {
        tone_mapping_pass_->Render();

        bool has_readback = ReadbackFinalOutputIfRequested(tone_mapping_rt_, false, RHIPipelineStage::ColorOutput);
        const bool has_readback_without_ui = has_readback;
        const bool rendered_ui = render_config_.render_ui && ui_pass_;

        if (rendered_ui)
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

        RHIPipelineStage final_after_stage = RHIPipelineStage::ColorOutput;
        if (has_readback || (!rendered_ui && has_readback_without_ui))
        {
            final_after_stage = RHIPipelineStage::Transfer;
        }

        tone_mapping_output_->Transition({.target_layout = RHIImageLayout::Read,
                                          .after_stage = final_after_stage,
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

PerformanceMetrics GPURenderer::GetLatestPerformanceMetrics() const
{
    return performance_monitor_.GetLatestMetrics();
}

void GPURenderer::Update()
{
    PROFILE_SCOPE("GPURenderer::Update");

    // Start convergence/performance measurement from the first fully-loaded scene frame.
    // This has to happen before the frame's spp/sample counters are prepared.
    if (scene_loaded_ && !scene_load_reset_done_)
    {
        auto *camera = scene_render_proxy_->GetCamera();
        camera->MarkPixelDirty();
        dispatched_sample_count_ = 0;
        if (reblur_path_)
        {
            reblur_path_->Reset();
        }
        performance_monitor_.Reset();
        scene_load_reset_done_ = true;
    }

    if (scene_render_proxy_->GetBindlessManager()->IsBufferDirty())
    {
        BindBindlessResources();
        if (reblur_path_)
        {
            reblur_path_->BindBindlessResources(*scene_render_proxy_->GetBindlessManager());
        }
    }

    auto *sky_light = scene_render_proxy_->GetSkyLight();
    if (sky_light != bound_sky_proxy_)
    {
        bound_sky_proxy_ = sky_light;

        // Reset accumulation when sky light changes so early frames rendered
        // with a dummy black cubemap don't drag down the running average.
        scene_render_proxy_->GetCamera()->MarkPixelDirty();
        dispatched_sample_count_ = 0;
        if (reblur_path_)
        {
            reblur_path_->ResetFinalHistory();
        }
        performance_monitor_.Reset();

        auto bind_sky_to_pipeline = [&](auto *resources) {
            if ((sky_light != nullptr) && sky_light->GetSkyMap())
            {
                auto sky_map = sky_light->GetSkyMap();
                resources->skyMap().BindResource(sky_map->GetDefaultView(rhi_));
                resources->skyMapSampler().BindResource(sky_map->GetSampler());
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
                resources->skyMap().BindResource(dummy_texture->GetDefaultView(rhi_));
                resources->skyMapSampler().BindResource(dummy_texture->GetSampler());
            }
        };

        bind_sky_to_pipeline(pipeline_state_->GetShaderResource<RayTracingComputeShader>());

        if (reblur_path_)
        {
            reblur_path_->BindSkyLight(sky_light);
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
        if (reblur_path_)
        {
            reblur_path_->ResetFinalHistory();
        }
        performance_monitor_.Reset();

        auto *cs_resources = pipeline_state_->GetShaderResource<RayTracingComputeShader>();
        cs_resources->tlas().BindResource(tlas_, true);

        if (reblur_path_)
        {
            reblur_path_->BindTlas(tlas_.get(), true);
        }
    }
    else if (!primitives_to_update.empty())
    {
        // non-structural change, update TLAS
        tlas_->Update(primitives_to_update);
        if (reblur_path_)
        {
            reblur_path_->ResetFinalHistory();
        }
        performance_monitor_.Reset();
    }

    auto *camera = scene_render_proxy_->GetCamera();
    if (camera->NeedClear())
    {
        dispatched_sample_count_ = 0;
        performance_monitor_.Reset();
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

    RayTracingComputeShader::UniformBufferData ubo{
        .camera = camera->GetUniformBufferData(render_config_),
        .time_seed = time_seed,
        .total_sample_count = camera->GetCumulatedSampleCount(),
        .spp = spp,
        .enable_nee = render_config_.enable_nee ? 1u : 0,
    };

    dispatched_sample_count_ += spp;
    current_frame_spp_ = spp;
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

    if (reblur_path_)
    {
        reblur_path_->UpdateFrameData(*camera, sky_light, dir_light,
                                      ReblurFrameParameters{
                                          .time_seed = time_seed,
                                          .total_sample_count = camera->GetCumulatedSampleCount(),
                                          .sample_per_pixel = spp,
                                          .dispatched_sample_count = dispatched_sample_count_,
                                          .enable_nee = render_config_.enable_nee,
                                          .debug_mode = render_config_.debug_mode,
                                      });
    }

    screen_quad_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    if (ui_pass_)
    {
        ui_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

    tone_mapping_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

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
    cs_resources->sampleStatsOutput().BindResource(performance_sample_stats_->GetDefaultView(rhi_));
    cs_resources->tlas().BindResource(tlas_);

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
