#include "renderer/renderer/GPURenderer.h"

#include "core/Profiler.h"
#include "renderer/BindlessManager.h"
#include "renderer/denoiser/Denoiser.h"
#include "renderer/denoiser/DenoiserFactory.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/ToneMappingPass.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/PathTracingDenoiserInputs.h"
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

static constexpr RHISampler::SamplerAttribute MaterialTextureSampler{
    .address_mode = RHISampler::SamplerAddressMode::Repeat,
    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest};

GPURenderer::GPURenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                         SceneRenderProxy *scene_render_proxy, RGTexturePool &graph_texture_pool)
    : Renderer(render_config, rhi_context, scene_render_proxy, graph_texture_pool),
      spp_logger_(1.f, false, [this](float) { MeasurePerformance(); })
{
    ASSERT_EQUAL(render_config.pipeline, RenderConfig::Pipeline::Gpu);

    ASSERT(rhi_->SupportsHardwareRayTracing());

    // preference order: MetalFX runs on the platform's own scaler where it exists, NRD is the
    // portable fallback. CreateDenoiser returns null for a provider this build cannot serve
    denoiser_slots_.push_back({.provider = DenoiserProvider::MetalFx, .denoiser = nullptr, .failed = false});
    denoiser_slots_.push_back({.provider = DenoiserProvider::Nrd, .denoiser = nullptr, .failed = false});
}

void GPURenderer::InitRenderResources()
{
    scene_render_proxy_->InitRenderResources(rhi_, render_config_);

    CreateAccumulator();

    denoiser_inputs_ = std::make_unique<PathTracingDenoiserInputs>(rhi_, resolution_.scene);

    InitSceneRenderResources();

    InitPostChain(ToneMappingPass::ScreenFormat, PostChain::ScreenPass::ToneMapping);
    displayed_image_ = scene_texture_;

    performance_history_.resize(rhi_->GetMaxFramesInFlight());

    running_time_per_spp_ =
        1000.f / render_config_.target_framerate / static_cast<float>(render_config_.sample_per_pixel);

    compute_pass_ = rhi_->CreateComputePass("GPURendererComputePass", true);
}

void GPURenderer::CreateAccumulator()
{
    scene_texture_ = rhi_->CreateImage(
        {
            .format = PixelFormat::RGBAFloat,
            .width = resolution_.scene.x(),
            .height = resolution_.scene.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV | RHIImage::ImageUsage::ColorAttachment,
            .memory_properties = RHIMemoryProperty::DeviceLocal,
            .mip_levels = 1,
            .msaa_samples = 1,
        },
        "Accumulator");
}

void GPURenderer::OnResize()
{
    CreateAccumulator();
    displayed_image_ = scene_texture_;

    denoiser_inputs_->Resize(resolution_.scene);

    // a provider that could not serve the previous extents is tried again at the new ones
    for (auto &slot : denoiser_slots_)
    {
        if (slot.denoiser)
        {
            slot.denoiser->Resize(resolution_.scene, resolution_.output);
        }
        slot.failed = slot.denoiser && !slot.denoiser->IsReady();
    }
}

RGTexture GPURenderer::BuildGraph(RenderGraph &graph)
{
    PROFILE_SCOPE("GPURenderer::BuildGraph");

    auto *camera = scene_render_proxy_->GetCamera();

    const auto accumulator = graph.Import("Accumulator", scene_texture_);
    const auto tlas = graph.Import("TLAS", tlas_);

    if (camera->NeedClear())
    {
        graph.AddRasterPass("ClearAccumulator", [accumulator](RGBuilder &builder) {
            builder.ColorWrite(accumulator, 0, Vector4::Zero());
            return [](RGRasterContext &) {};
        });
        camera->ClearPixels();
    }

    // Once the target sample count is reached the image has converged; stop accumulating so the
    // result is stable and deterministic (e.g. for screenshots) instead of drifting on fresh noise.
    const bool accumulation_complete =
        !camera->NeedClear() && camera->GetCumulatedSampleCount() >= render_config_.max_sample_per_pixel;

    auto tone_mapping_input = accumulator;

    if (tlas_->HasStagedBuild())
    {
        graph.AddCopyPass("BuildTLAS", [tlas](RGBuilder &builder) {
            builder.AccelerationStructureBuild(tlas);
            return [tlas](RGCopyContext &context) { context.BuildAccelerationStructure(tlas); };
        });
    }

    // base pass: render to texture
    if (tlas_->HasInstances() && !accumulation_complete && !AccumulationPaused())
    {
        std::optional<DenoiserInputs> denoiser_inputs;
        if (denoiser_inputs_->IsAllocated())
        {
            denoiser_inputs = denoiser_inputs_->Import(graph, accumulator);
        }

        graph.AddComputePass(
            "PathTrace", compute_pass_, [this, accumulator, tlas, denoiser_inputs](RGBuilder &builder) {
                using Table = RayTracingComputeShader::ResourceTable;
                builder.AccelerationStructureRead(tlas, &Table::tlas);
                builder.StorageReadWrite(accumulator, &Table::imageData);
                // the tracer binds them as storage images on every dispatch, written or not
                if (denoiser_inputs)
                {
                    for (const auto &[texture, binding] :
                         {std::pair{denoiser_inputs->noisy_radiance_hit_distance, &Table::gRadiance},
                          std::pair{denoiser_inputs->normal_view_depth, &Table::gNormalDepth},
                          std::pair{denoiser_inputs->albedo_object_id, &Table::gAlbedoObj},
                          std::pair{denoiser_inputs->motion_hit_metallic, &Table::gMotion},
                          std::pair{denoiser_inputs->noisy_specular_radiance_hit_distance, &Table::gRadianceSpecular},
                          std::pair{denoiser_inputs->specular_albedo_roughness, &Table::gSpecAlbedo}})
                    {
                        builder.StorageWrite(texture, binding);
                    }
                }
                return [this](RGComputeContext &context) {
                    context.DispatchCompute(pipeline_state_, {resolution_.scene.x(), resolution_.scene.y(), 1u},
                                            {16u, 16u, 1u});
                };
            });

        // an encoded frame is displayed as-is even when it completes max_spp: the max_spp=1 motion
        // harnesses film the denoiser, and NRD's final resolve equals the accumulator bit-exactly
        if (gbuffer_write_this_frame_ && denoiser_inputs)
        {
            tone_mapping_input = frame_denoiser_->AddTo(graph, *denoiser_inputs);
            displayed_image_ = tone_mapping_input == accumulator ? scene_texture_ : frame_denoiser_->GetOutput();
        }
        else
        {
            displayed_image_ = scene_texture_;
        }
    }
    else if (displayed_image_ != scene_texture_)
    {
        // a frame without a dispatch keeps displaying the last denoised output
        tone_mapping_input = graph.Import("DenoiserOutput", displayed_image_);
    }

    return tone_mapping_input;
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
    Denoiser *selected_denoiser = SelectDenoiser(requested);

    const bool selection_changed = requested != requested_provider_ || selected_denoiser != frame_denoiser_;
    requested_provider_ = requested;
    frame_denoiser_ = selected_denoiser;
    denoiser_reset_this_frame_ = selection_changed;

    if (requested == DenoiserProvider::Off)
    {
        displayed_image_ = scene_texture_;
    }
    else if (selection_changed)
    {
        camera->MarkPixelDirty();
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
        }
        else
        {
            auto dummy_texture = rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
                .format = PixelFormat::RGBAFloat16,
                .usages = RHIImage::ImageUsage::Texture,
                .type = RHIImage::ImageType::Image2DCube,
            });
            cs_resources->skyMap().BindResource(dummy_texture->GetDefaultView(rhi_));
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

    // the BuildTLAS pass records the staged build
    if (need_rebuild_tlas)
    {
        // structural change, rebuild TLAS
        tlas_->Build();
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
            .delta_time = render_config_.delta_time,
            .accumulated_samples = camera->GetCumulatedSampleCount(),
            .maximum_samples = render_config_.max_sample_per_pixel,
            .reset_history = denoiser_reset_this_frame_ || !scene_ready,
            .final_frame = final_frame_this_frame_,
        });
    }
    gbuffer_write_this_frame_ = will_dispatch && frame_denoiser_ != nullptr && frame_denoiser_->NeedsInputs();

    if (gbuffer_write_this_frame_)
    {
        denoiser_inputs_->EnsureAllocated(DenoiserConfig::Get().radiance_fp16 ? PixelFormat::RGBAFloat16
                                                                              : PixelFormat::RGBAFloat);
    }

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

    // the dummies the tracer binds until the path trace pass declares allocated inputs
    BindDenoiserInputs();

    auto dummy_texture_cube = rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
        .format = PixelFormat::RGBAFloat16,
        .usages = RHIImage::ImageUsage::Texture,
        .type = RHIImage::ImageType::Image2DCube,
    });

    cs_resources->skyMap().BindResource(dummy_texture_cube->GetDefaultView(rhi_));
    cs_resources->skyMapSampler().BindResource(rhi_->GetSampler(SkyRenderProxy::SkyMapSampler));

    cs_resources->materialTextureSampler().BindResource(rhi_->GetSampler(MaterialTextureSampler));

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

GPURenderer::DenoiserSlot *GPURenderer::FindDenoiserSlot(DenoiserProvider provider)
{
    auto slot = std::ranges::find_if(
        denoiser_slots_, [provider](const DenoiserSlot &candidate) { return candidate.provider == provider; });
    return slot == denoiser_slots_.end() ? nullptr : &*slot;
}

Denoiser *GPURenderer::GetOrCreateDenoiser(DenoiserProvider provider)
{
    DenoiserSlot *slot = FindDenoiserSlot(provider);
    if (!slot || slot->failed)
    {
        return nullptr;
    }

    if (slot->denoiser && !slot->denoiser->IsReady())
    {
        slot->failed = true;
        Log(Error, "Denoiser {} failed while encoding; selecting a fallback", slot->denoiser->GetName());
        return nullptr;
    }

    if (!slot->denoiser)
    {
        const DenoiserConfig &config = DenoiserConfig::Get();
        DenoiserDesc desc{
            .input_size = resolution_.scene,
            .output_size = resolution_.output,
            .radiance_format = config.radiance_fp16 ? PixelFormat::RGBAFloat16 : PixelFormat::RGBAFloat,
            .accumulator_format = scene_texture_->GetAttributes().format,
            .max_frames_in_flight = rhi_->GetMaxFramesInFlight(),
            .synchronous_initialization = config.metalfx_sync_init,
        };
        slot->denoiser = CreateDenoiser(provider, desc, rhi_);
        if (!slot->denoiser || !slot->denoiser->IsReady())
        {
            slot->failed = true;
            Log(Warn, "Denoiser provider {} is unavailable for {}x{} -> {}x{}", Enum2Str(provider), desc.input_size.x(),
                desc.input_size.y(), desc.output_size.x(), desc.output_size.y());
            slot->denoiser.reset();
            return nullptr;
        }
    }
    return slot->denoiser.get();
}

Denoiser *GPURenderer::SelectDenoiser(DenoiserProvider requested)
{
    if (requested == DenoiserProvider::Off)
    {
        return nullptr;
    }

    // Auto starts at the most preferred provider; an explicit request starts at itself and
    // still falls back down the preference order when it is unavailable
    size_t begin = 0;
    if (requested != DenoiserProvider::Auto)
    {
        const DenoiserSlot *slot = FindDenoiserSlot(requested);
        if (!slot)
        {
            return nullptr;
        }
        begin = static_cast<size_t>(slot - denoiser_slots_.data());
    }

    for (size_t index = begin; index < denoiser_slots_.size(); index++)
    {
        const DenoiserProvider provider = denoiser_slots_[index].provider;
        if (Denoiser *denoiser = GetOrCreateDenoiser(provider))
        {
            return denoiser;
        }
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
