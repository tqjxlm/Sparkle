#include "renderer/renderer/GPURenderer.h"

#include "core/Profiler.h"
#include "renderer/BindlessManager.h"
#include "renderer/nrd/NrdDenoiser.h"
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
    REGISTGER_SHADER(RayTracingComputeShader, RHIShaderStage::Compute, "shaders/ray_trace/ray_trace.cs.slang",
                     "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(tlas, RHIShaderResourceReflection::ResourceType::AccelerationStructure)
    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(imageData, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(materialIdBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(materialBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(skyMap, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(skyMapSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(materialTextureSampler, RHIShaderResourceReflection::ResourceType::Sampler)

    USE_SHADER_RESOURCE(gRadiance, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(gNormalDepth, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(gAlbedoObj, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(gMotion, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(gRadianceSpecular, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(gSpecAlbedo, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    USE_SHADER_RESOURCE_BINDLESS(textures, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE_BINDLESS(indexBuffers, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE_BINDLESS(vertexAttributeBuffers, RHIShaderResourceReflection::ResourceType::StorageBuffer)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        CameraRenderProxy::UniformBufferData camera;
        SkyRenderProxy::UniformBufferData sky_light = {};
        DirectionalLightRenderProxy::UniformBufferData dir_light = {};
        Mat4 view_projection;
        Mat4 prev_view_projection;
        uint32_t time_seed;
        float output_limit = CameraRenderProxy::OutputLimit;
        uint32_t total_sample_count;
        uint32_t spp;
        uint32_t enable_nee;
        uint32_t write_gbuffer;
    };
};

GPURenderer::GPURenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                         SceneRenderProxy *scene_render_proxy)
    : Renderer(render_config, rhi_context, scene_render_proxy),
      spp_logger_(1.f, false, [this](float) { MeasurePerformance(); })
{
    ASSERT_EQUAL(render_config.pipeline, RenderConfig::Pipeline::gpu);

    ASSERT(rhi_->SupportsHardwareRayTracing());
}

void GPURenderer::InitRenderResources()
{
    scene_render_proxy_->InitRenderResources(rhi_, render_config_);

    RHIRenderTarget::Attribute scene_rt_attribute;
    scene_rt_attribute.SetColorAttribute(
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
        0);

    scene_rt_ = rhi_->GetRenderTargetPool().Acquire(scene_rt_attribute, "GPUPipelineColorRT");
    scene_texture_ = scene_rt_->GetColorImage(0);

    RHIRenderTarget::Attribute tone_mapping_rt_attribute;
    tone_mapping_rt_attribute.SetColorAttribute(
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
        0);

    tone_mapping_rt_ = rhi_->GetRenderTargetPool().Acquire(tone_mapping_rt_attribute, "ToneMappingRT");
    tone_mapping_output_ = tone_mapping_rt_->GetColorImage(0);

    // Create the denoiser before the path-tracer pipeline so its G-buffer targets exist and can
    // be bound into the compute shader in InitSceneRenderResources().
    nrd_ = PipelinePass::Create<NrdDenoiser>(render_config_, rhi_, scene_texture_);

    InitSceneRenderResources();

    nrd_enabled_last_ = nrd_->IsActive() && nrd_->GetOutput();
    auto tone_mapping_input = nrd_enabled_last_ ? nrd_->GetOutput() : scene_texture_;

    tone_mapping_pass_ =
        PipelinePass::Create<ToneMappingPass>(render_config_, rhi_, tone_mapping_input, tone_mapping_rt_);

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
}

void GPURenderer::Render()
{
    PROFILE_SCOPE("GPURenderer::Render");

    auto *camera = scene_render_proxy_->GetCamera();

    bool nrd_on = nrd_frame_active_;
    if (nrd_on != nrd_enabled_last_)
    {
        if (nrd_on)
        {
            nrd_->EnsureEnabledResources();
            BindNrdGBuffer();
            nrd_->RequestReset(); // freshly-allocated history is garbage; start clean
            // enabling after the render freeze: no pass would ever write the NRD output, so restart
            // accumulation (it re-converges through the handoff)
            if (!camera->NeedClear() && camera->GetCumulatedSampleCount() >= render_config_.max_sample_per_pixel)
            {
                nrd_enable_restart_pending_ = true;
            }
            // this frame's Update snapshot predates the enable (write_gbuffer = 0), so the dispatch
            // would leave the fresh G-buffer unwritten and its undefined contents would poison
            // ReBLUR's history: activate only on a frame that writes it
            nrd_on = gbuffer_write_this_frame_;
        }
        nrd_on = nrd_on && nrd_->GetOutput();
        tone_mapping_pass_->SetInput(nrd_on ? nrd_->GetOutput() : scene_texture_);
        nrd_enabled_last_ = nrd_on;
    }
    nrd_on = nrd_enabled_last_;

    if (camera->NeedClear())
    {
        clear_pass_->Render();
        camera->ClearPixels();
    }

    // Once the target sample count is reached the image has converged; stop accumulating so the
    // result is stable and deterministic (e.g. for screenshots) instead of drifting on fresh noise.
    const bool accumulation_complete =
        !camera->NeedClear() && camera->GetCumulatedSampleCount() >= render_config_.max_sample_per_pixel;

    // Frames rendered before the scene is fully loaded are dark/incomplete; keep resetting denoiser
    // history until then so they never enter the temporal accumulation. Once ready, NRD accumulates
    // (and reprojects under camera motion) normally — camera movement does not force a history reset.
    if (!Renderer::IsReadyForAutoScreenshot())
    {
        nrd_->RequestReset();
    }

    // base pass: render to texture
    if (!accumulation_complete && !AccumulationPaused())
    {
        scene_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                    .after_stage = RHIPipelineStage::Top,
                                    .before_stage = RHIPipelineStage::ComputeShader});

        // once the real G-buffer targets exist they stay bound to this pipeline as storage images, so
        // every dispatch needs them in a writable layout, even when the shader skips the write
        if (nrd_->GetOutput())
        {
            nrd_->BeginGBufferWrite();
        }

        rhi_->BeginComputePass(compute_pass_);

        rhi_->DispatchCompute(pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});

        rhi_->EndComputePass(compute_pass_);

        const auto scene_consumer_stage = nrd_on ? RHIPipelineStage::ComputeShader : RHIPipelineStage::PixelShader;

        scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = scene_consumer_stage});

        if (nrd_on)
        {
            nrd_->Render();
        }
    }

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
        if (has_readback)
        {
            final_after_stage = RHIPipelineStage::Transfer;
        }
        else if (!rendered_ui && has_readback_without_ui)
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

void GPURenderer::Update()
{
    PROFILE_SCOPE("GPURenderer::Update");

    nrd_->SampleConfig();
    nrd_frame_active_ = nrd_->IsActive();

    if (nrd_enable_restart_pending_)
    {
        nrd_enable_restart_pending_ = false;
        scene_render_proxy_->GetCamera()->MarkPixelDirty();
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

        // Reset accumulation when sky light changes so early frames rendered
        // with a dummy black cubemap don't drag down the running average.
        scene_render_proxy_->GetCamera()->MarkPixelDirty();

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

    auto *camera = scene_render_proxy_->GetCamera();

    // Restart accumulation from seed 0 when the scene finishes loading: the warm-up frame count is
    // I/O-timing dependent, and converged captures must be bit-reproducible run-to-run.
    const bool scene_ready = Renderer::IsReadyForAutoScreenshot();
    if (scene_ready && !scene_ready_last_)
    {
        seed_counter_ = 0;
        camera->MarkPixelDirty();
    }
    scene_ready_last_ = scene_ready;

    // The seed advances every dispatch and must NOT reset on accumulator clears: deriving it from a
    // per-clear counter replays the SAME noise every frame under camera motion (clear per frame ->
    // seed pinned), and temporal denoisers cannot average correlated noise.
    auto time_seed = seed_counter_ + render_config_.random_seed_offset;

    nrd_->UpdateFrameData(render_config_, scene_render_proxy_);
    gbuffer_write_this_frame_ = nrd_frame_active_ && nrd_->NeedsGBuffer();

    const auto frame_index = rhi_->GetFrameIndex();

    // must match Render()'s dispatch gate: frozen and manually-paused frames skip the trace dispatch
    const bool will_dispatch =
        !AccumulationPaused() &&
        (camera->NeedClear() || camera->GetCumulatedSampleCount() < render_config_.max_sample_per_pixel);

    uint32_t spp = render_config_.sample_per_pixel;

    if (render_config_.use_dynamic_spp)
    {
        auto gpu_time = compute_pass_->GetExecutionTime(frame_index);

        // the estimate is only updatable when the slot's timestamp and spp come from the same real
        // dispatch; spp == 0 marks "no dispatch used this slot" (startup or frozen frames)
        if (gpu_time > 0 && performance_history_[frame_index].spp > 0)
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
    }

    // recorded regardless of mode so toggling dynamic_spp never pairs a timestamp with the wrong spp
    performance_history_[frame_index].spp = will_dispatch ? spp : 0;

    RayTracingComputeShader::UniformBufferData ubo{
        .camera = camera->GetUniformBufferData(render_config_),
        .view_projection = camera->GetViewProjectionMatrix(),
        .prev_view_projection = camera->GetPrevViewProjectionMatrix(),
        .time_seed = time_seed,
        .total_sample_count = camera->GetCumulatedSampleCount(),
        .spp = spp,
        .enable_nee = render_config_.enable_nee ? 1u : 0,
        .write_gbuffer = gbuffer_write_this_frame_ ? 1u : 0u,
    };

    if (will_dispatch)
    {
        seed_counter_ += spp;
        camera->AccumulateSample(spp);
    }

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

    last_second_total_spp_ += spp;

    spp_logger_.Tick();

    Logger::LogToScreen("Accumulation", fmt::format("Accumulated samples: {}", camera->GetCumulatedSampleCount()));
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

    BindNrdGBuffer();

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

void GPURenderer::BindNrdGBuffer()
{
    auto *cs_resources = pipeline_state_->GetShaderResource<RayTracingComputeShader>();
    cs_resources->gRadiance().BindResource(nrd_->GetRadiance()->GetDefaultView(rhi_));
    cs_resources->gNormalDepth().BindResource(nrd_->GetNormalDepth()->GetDefaultView(rhi_));
    cs_resources->gAlbedoObj().BindResource(nrd_->GetAlbedoObj()->GetDefaultView(rhi_));
    cs_resources->gMotion().BindResource(nrd_->GetMotion()->GetDefaultView(rhi_));
    cs_resources->gRadianceSpecular().BindResource(nrd_->GetRadianceSpecular()->GetDefaultView(rhi_));
    cs_resources->gSpecAlbedo().BindResource(nrd_->GetSpecAlbedo()->GetDefaultView(rhi_));
}

void GPURenderer::BindBindlessResources()
{
    auto *cs_resources = pipeline_state_->GetShaderResource<RayTracingComputeShader>();

    const auto *bindless_manager = scene_render_proxy_->GetBindlessManager();

    cs_resources->materialIdBuffer().BindResource(bindless_manager->GetMaterialIdBuffer());
    cs_resources->materialBuffer().BindResource(bindless_manager->GetMaterialParameterBuffer());

    cs_resources->textures().BindResource(bindless_manager->GetBindlessBuffer(BindlessResourceType::Texture));
    cs_resources->indexBuffers().BindResource(bindless_manager->GetBindlessBuffer(BindlessResourceType::IndexBuffer));
    cs_resources->vertexAttributeBuffers().BindResource(
        bindless_manager->GetBindlessBuffer(BindlessResourceType::VertexAttributeBuffer));
}
} // namespace sparkle
