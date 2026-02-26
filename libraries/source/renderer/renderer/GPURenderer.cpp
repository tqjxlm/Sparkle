#include "renderer/renderer/GPURenderer.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "renderer/BindlessManager.h"
#include "renderer/denoiser/ReblurDenoiser.h"
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

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        Vector2UInt resolution;
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
        InitReblurResources();
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
        camera->ClearPixels();
        dispatched_sample_count_ = 0;
        if (reblur_)
        {
            reblur_->Reset();
        }
    }

    if (reblur_)
    {
        RenderReblurPath();
    }
    else
    {
        // Original path: render to texture
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

    if (scene_render_proxy_->GetBindlessManager()->IsBufferDirty())
    {
        BindBindlessResources();
        if (reblur_)
        {
            BindSplitBindlessResources();
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

        if (reblur_)
        {
            bind_sky_to_pipeline(split_pt_pipeline_->GetShaderResource<SplitPathTracerShader>());
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

        if (reblur_)
        {
            auto *split_resources = split_pt_pipeline_->GetShaderResource<SplitPathTracerShader>();
            split_resources->tlas().BindResource(tlas_, true);
        }
    }
    else if (!primitives_to_update.empty())
    {
        // non-structural change, update TLAS
        tlas_->Update(primitives_to_update);
    }

    auto *camera = scene_render_proxy_->GetCamera();

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

    if (reblur_)
    {
        SplitPathTracerShader::UniformBufferData split_ubo{
            .camera = ubo.camera,
            .sky_light = ubo.sky_light,
            .dir_light = ubo.dir_light,
            .time_seed = ubo.time_seed,
            .total_sample_count = ubo.total_sample_count,
            .spp = ubo.spp,
            .enable_nee = ubo.enable_nee,
        };
        // view_matrix and hit_dist_params use defaults for now
        split_pt_uniform_buffer_->Upload(rhi_, &split_ubo);

        ReblurCompositeShader::UniformBufferData comp_ubo{
            .resolution = {image_size_.x(), image_size_.y()},
        };
        composite_uniform_buffer_->Upload(rhi_, &comp_ubo);
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

void GPURenderer::BindSplitBindlessResources()
{
    auto *resources = split_pt_pipeline_->GetShaderResource<SplitPathTracerShader>();

    const auto *bindless_manager = scene_render_proxy_->GetBindlessManager();

    resources->materialIdBuffer().BindResource(bindless_manager->GetMaterialIdBuffer());
    resources->materialBuffer().BindResource(bindless_manager->GetMaterialParameterBuffer());

    resources->textures().BindResource(bindless_manager->GetBindlessBuffer(BindlessResourceType::Texture));
    resources->indexBuffers().BindResource(bindless_manager->GetBindlessBuffer(BindlessResourceType::IndexBuffer));
    resources->vertexBuffers().BindResource(bindless_manager->GetBindlessBuffer(BindlessResourceType::VertexBuffer));
    resources->vertexAttributeBuffers().BindResource(
        bindless_manager->GetBindlessBuffer(BindlessResourceType::VertexAttributeBuffer));
}

void GPURenderer::InitReblurResources()
{
    auto width = image_size_.x();
    auto height = image_size_.y();

    auto make_aux_image = [this, width, height](PixelFormat format, const std::string &name) {
        return rhi_->CreateImage(
            RHIImage::Attribute{
                .format = format,
                .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                            .filtering_method_min = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
                .width = width,
                .height = height,
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV | RHIImage::ImageUsage::TransferSrc,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
            },
            name);
    };

    diffuse_signal_ = make_aux_image(PixelFormat::RGBAFloat16, "ReblurDiffuseSignal");
    specular_signal_ = make_aux_image(PixelFormat::RGBAFloat16, "ReblurSpecularSignal");
    normal_roughness_ = make_aux_image(PixelFormat::RGBAFloat16, "ReblurNormalRoughness");
    view_z_ = make_aux_image(PixelFormat::R32_FLOAT, "ReblurViewZ");
    motion_vectors_ = make_aux_image(PixelFormat::RG16Float, "ReblurMotionVectors");
    albedo_metallic_ = make_aux_image(PixelFormat::RGBAFloat16, "ReblurAlbedoMetallic");

    // Split path tracer pipeline
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

    split_resources->skyMap().BindResource(dummy_texture_cube->GetDefaultView(rhi_));
    split_resources->skyMapSampler().BindResource(dummy_texture_cube->GetSampler());
    split_resources->materialTextureSampler().BindResource(dummy_texture_2d->GetSampler());

    BindSplitBindlessResources();

    // Composite pipeline
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

    // Compute pass for REBLUR dispatches (no timestamp to avoid query reuse within frame)
    reblur_compute_pass_ = rhi_->CreateComputePass("ReblurGPUComputePass", false);

    // Create REBLUR denoiser
    reblur_ = std::make_unique<ReblurDenoiser>(rhi_, width, height);
}

void GPURenderer::RenderReblurPath()
{
    PROFILE_SCOPE("GPURenderer::RenderReblurPath");

    // Transition auxiliary buffers to StorageWrite
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

    // Dispatch split path tracer
    rhi_->BeginComputePass(reblur_compute_pass_);
    rhi_->DispatchCompute(split_pt_pipeline_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(reblur_compute_pass_);

    // Debug pass 255: passthrough merge — skip denoiser and composite,
    // use the split shader's imageData accumulation directly
    if (render_config_.reblur_debug_pass == 255)
    {
        scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::PixelShader});
        return;
    }

    // Transition auxiliary buffers to Read for denoiser
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

    // Run REBLUR denoiser
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
    reblur_->Denoise(inputs, settings, matrices, dispatched_sample_count_, render_config_.reblur_debug_pass);

    // Bind denoised output to composite and dispatch
    auto *comp_resources = composite_pipeline_->GetShaderResource<ReblurCompositeShader>();
    comp_resources->denoisedDiffuse().BindResource(reblur_->GetDenoisedDiffuse()->GetDefaultView(rhi_));
    comp_resources->denoisedSpecular().BindResource(reblur_->GetDenoisedSpecular()->GetDefaultView(rhi_));
    comp_resources->albedoMetallic().BindResource(albedo_metallic_->GetDefaultView(rhi_));

    // Transition denoised output to Read and scene_texture to StorageWrite for composite
    reblur_->GetDenoisedDiffuse()->Transition({.target_layout = RHIImageLayout::Read,
                                               .after_stage = RHIPipelineStage::ComputeShader,
                                               .before_stage = RHIPipelineStage::ComputeShader});
    reblur_->GetDenoisedSpecular()->Transition({.target_layout = RHIImageLayout::Read,
                                                .after_stage = RHIPipelineStage::ComputeShader,
                                                .before_stage = RHIPipelineStage::ComputeShader});
    scene_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});

    // Dispatch composite
    rhi_->BeginComputePass(reblur_compute_pass_);
    rhi_->DispatchCompute(composite_pipeline_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(reblur_compute_pass_);

    // Transition scene_texture to Read for tone mapping
    scene_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::PixelShader});
}
} // namespace sparkle
