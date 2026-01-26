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
      spp_logger_(1.f, false, [this](float) { MeasurePerformance(); })
{
    ASSERT_EQUAL(render_config.pipeline, RenderConfig::Pipeline::gpu);

    ASSERT(rhi_->SupportsHardwareRayTracing());
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
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::ColorAttachment,
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

    if (camera->NeedClear())
    {
        clear_pass_->Render();
        camera->ClearPixels();
    }

    // base pass: render to texture
    {
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

        if (render_config_.render_ui)
        {
            ui_pass_->Render();
        }

        tone_mapping_output_->Transition({.target_layout = RHIImageLayout::Read,
                                          .after_stage = RHIPipelineStage::ColorOutput,
                                          .before_stage = RHIPipelineStage::PixelShader});
    }

    // screen pass: render texture on a screen quad
    screen_quad_pass_->Render();
}

GPURenderer::~GPURenderer() = default;

void GPURenderer::Update()
{
    PROFILE_SCOPE("GPURenderer::Update");

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

    auto *camera = scene_render_proxy_->GetCamera();

    auto current_time =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
    // the truncation is on purpose. we do not care about the actual value.
    auto time_seed = static_cast<uint32_t>(current_time.count());

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
} // namespace sparkle
