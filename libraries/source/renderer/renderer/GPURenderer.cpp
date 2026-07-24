#include "renderer/renderer/GPURenderer.h"

#include "core/Profiler.h"
#include "renderer/BindlessManager.h"
#include "renderer/denoiser/DenoiserFactory.h"
#include "renderer/pass/ClearTexturePass.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/pass/ToneMappingPass.h"
#include "renderer/pass/UiPass.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/PathTracingDenoiserInputs.h"
#include "rhi/RHI.h"
#include "rhi/RHIDenoiser.h"
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
    ASSERT_EQUAL(render_config.pipeline, RenderConfig::Pipeline::Gpu);

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
            .width = resolution_.scene.x(),
            .height = resolution_.scene.y(),
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
            .format = PixelFormat::B8G8R8A8Srgb,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = resolution_.output.x(),
            .height = resolution_.output.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::ColorAttachment |
                      RHIImage::ImageUsage::TransferSrc,
            .msaa_samples = 1,
        },
        0);

    tone_mapping_rt_ = rhi_->GetRenderTargetPool().Acquire(tone_mapping_rt_attribute, "ToneMappingRT");
    tone_mapping_output_ = tone_mapping_rt_->GetColorImage(0);

    denoiser_inputs_ = std::make_unique<PathTracingDenoiserInputs>(rhi_, resolution_.scene);

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
}

void GPURenderer::Render()
{
    PROFILE_SCOPE("GPURenderer::Render");

    auto *camera = scene_render_proxy_->GetCamera();

    if (camera->NeedClear())
    {
        clear_pass_->Render();
        camera->ClearPixels();
    }

    // Once the target sample count is reached the image has converged; stop accumulating so the
    // result is stable and deterministic (e.g. for screenshots) instead of drifting on fresh noise.
    const bool accumulation_complete =
        !camera->NeedClear() && camera->GetCumulatedSampleCount() >= render_config_.max_sample_per_pixel;

    // base pass: render to texture
    if (tlas_->HasInstances() && !accumulation_complete && !AccumulationPaused())
    {
        scene_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                    .after_stage = RHIPipelineStage::Top,
                                    .before_stage = RHIPipelineStage::ComputeShader});

        if (denoiser_inputs_->IsAllocated())
        {
            denoiser_inputs_->BeginWrite();
        }

        rhi_->BeginComputePass(compute_pass_);

        rhi_->DispatchCompute(pipeline_state_, {resolution_.scene.x(), resolution_.scene.y(), 1u}, {16u, 16u, 1u});

        rhi_->EndComputePass(compute_pass_);

        const auto scene_consumer_stage =
            frame_denoiser_ ? RHIPipelineStage::ComputeShader : RHIPipelineStage::PixelShader;

        scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = scene_consumer_stage});

        // an encoded frame is displayed as-is even when it completes max_spp: the max_spp=1 motion
        // harnesses film the denoiser, and NRD's final resolve equals the accumulator bit-exactly
        if (frame_denoiser_ && !gbuffer_write_this_frame_)
        {
            tone_mapping_pass_->SetInput(scene_texture_);
        }
        else if (frame_denoiser_ && gbuffer_write_this_frame_)
        {
            const bool encoded = frame_denoiser_->Encode(denoiser_inputs_->GetInputs(scene_texture_.get()));
            if (encoded && frame_denoiser_->GetOutput())
            {
                tone_mapping_pass_->SetInput(frame_denoiser_->GetOutput());
            }
            else
            {
                if (frame_provider_ == DenoiserProvider::Nrd)
                {
                    nrd_failed_ = true;
                }
                else if (frame_provider_ == DenoiserProvider::MetalFx)
                {
                    metalfx_failed_ = true;
                }
                Log(Error, "Denoiser {} failed while encoding; the next frame will select a fallback",
                    frame_denoiser_->GetName());
            }
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

void GPURenderer::Update()
{
    PROFILE_SCOPE("GPURenderer::Update");

    auto *camera = scene_render_proxy_->GetCamera();
    const DenoiserProvider requested = DenoiserConfig::Get().provider;
    DenoiserProvider selected_provider = DenoiserProvider::Off;
    RHIDenoiser *selected_denoiser = SelectDenoiser(requested, selected_provider);

    const bool selection_changed = requested != requested_provider_ || selected_denoiser != frame_denoiser_;
    requested_provider_ = requested;
    frame_provider_ = selected_provider;
    frame_denoiser_ = selected_denoiser;
    denoiser_reset_this_frame_ = selection_changed;

    if (requested == DenoiserProvider::Off)
    {
        tone_mapping_pass_->SetInput(scene_texture_);
    }
    else if (selection_changed)
    {
        camera->MarkPixelDirty();
    }

    if (frame_denoiser_ && frame_denoiser_->NeedsInputs() &&
        denoiser_inputs_->EnsureAllocated(DenoiserConfig::Get().radiance_fp16 ? PixelFormat::RGBAFloat16
                                                                              : PixelFormat::RGBAFloat))
    {
        BindDenoiserInputs();
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
    const auto &primitive_changes = scene_render_proxy_->GetPrimitiveChangeList();
    if (!primitive_changes.empty())
    {
        denoiser_reset_this_frame_ = true;
    }
    for (const auto &[type, primitive, from, to] : primitive_changes)
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

    const auto frame_index = rhi_->GetFrameIndex();

    // must match Render()'s dispatch gate: frozen and manually-paused frames skip the trace dispatch
    const bool will_dispatch =
        tlas_->HasInstances() && !AccumulationPaused() &&
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

    final_frame_this_frame_ =
        will_dispatch && camera->GetCumulatedSampleCount() + spp >= render_config_.max_sample_per_pixel;
    if (frame_denoiser_)
    {
        frame_denoiser_->UpdateFrameData({
            .view = camera->GetViewMatrix(),
            .projection = camera->GetProjectionMatrix(),
            .exposure = camera->GetAttribute().exposure,
            .far_plane = camera->GetFar(),
            .accumulated_samples = camera->GetCumulatedSampleCount(),
            .maximum_samples = render_config_.max_sample_per_pixel,
            .reset_history = denoiser_reset_this_frame_ || !scene_ready,
            .final_frame = final_frame_this_frame_,
        });
    }
    gbuffer_write_this_frame_ = will_dispatch && frame_denoiser_ != nullptr && frame_denoiser_->NeedsInputs();

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
        last_second_total_spp_ += spp;
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

    BindDenoiserInputs();

    auto dummy_texture_2d = rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
        .format = PixelFormat::R8G8B8A8Srgb,
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

void GPURenderer::BindDenoiserInputs()
{
    auto *cs_resources = pipeline_state_->GetShaderResource<RayTracingComputeShader>();
    cs_resources->gRadiance().BindResource(denoiser_inputs_->GetNoisyRadianceHitDistance()->GetDefaultView(rhi_));
    cs_resources->gNormalDepth().BindResource(denoiser_inputs_->GetNormalViewDepth()->GetDefaultView(rhi_));
    cs_resources->gAlbedoObj().BindResource(denoiser_inputs_->GetAlbedoObjectId()->GetDefaultView(rhi_));
    cs_resources->gMotion().BindResource(denoiser_inputs_->GetMotionHitMetallic()->GetDefaultView(rhi_));
    cs_resources->gRadianceSpecular().BindResource(
        denoiser_inputs_->GetNoisySpecularRadianceHitDistance()->GetDefaultView(rhi_));
    cs_resources->gSpecAlbedo().BindResource(denoiser_inputs_->GetSpecularAlbedoRoughness()->GetDefaultView(rhi_));
}

RHIDenoiser *GPURenderer::GetOrCreateDenoiser(DenoiserProvider provider)
{
    std::unique_ptr<RHIDenoiser> *slot = nullptr;
    bool *failed = nullptr;
    if (provider == DenoiserProvider::Nrd)
    {
        slot = &nrd_denoiser_;
        failed = &nrd_failed_;
    }
    else if (provider == DenoiserProvider::MetalFx)
    {
        slot = &metalfx_denoiser_;
        failed = &metalfx_failed_;
    }
    else
    {
        return nullptr;
    }

    if (*failed)
    {
        return nullptr;
    }
    if (!*slot)
    {
        const DenoiserConfig &config = DenoiserConfig::Get();
        RHIDenoiserDesc desc{
            .input_size = resolution_.scene,
            .output_size = resolution_.output,
            .radiance_format = config.radiance_fp16 ? PixelFormat::RGBAFloat16 : PixelFormat::RGBAFloat,
            .max_frames_in_flight = rhi_->GetMaxFramesInFlight(),
            .synchronous_initialization = config.metalfx_sync_init,
        };
        *slot = CreateDenoiser(provider, desc, rhi_);
        if (!*slot || !(*slot)->IsReady())
        {
            *failed = true;
            Log(Warn, "Denoiser provider {} is unavailable for {}x{} -> {}x{}",
                provider == DenoiserProvider::MetalFx ? "MetalFX" : "NRD", desc.input_size.x(), desc.input_size.y(),
                desc.output_size.x(), desc.output_size.y());
            slot->reset();
            return nullptr;
        }
    }
    return slot->get();
}

RHIDenoiser *GPURenderer::SelectDenoiser(DenoiserProvider requested, DenoiserProvider &effective)
{
    effective = DenoiserProvider::Off;
    if (requested == DenoiserProvider::Off)
    {
        return nullptr;
    }

    if (requested == DenoiserProvider::MetalFx || requested == DenoiserProvider::Auto)
    {
        if (RHIDenoiser *denoiser = GetOrCreateDenoiser(DenoiserProvider::MetalFx))
        {
            effective = DenoiserProvider::MetalFx;
            return denoiser;
        }
    }

    if (RHIDenoiser *denoiser = GetOrCreateDenoiser(DenoiserProvider::Nrd))
    {
        effective = DenoiserProvider::Nrd;
        return denoiser;
    }
    return nullptr;
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
