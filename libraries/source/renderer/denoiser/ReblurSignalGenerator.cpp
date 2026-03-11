#include "renderer/denoiser/ReblurSignalGenerator.h"

#include "renderer/BindlessManager.h"
#include "renderer/pass/ClearTexturePass.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"

#include <utility>

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

[[maybe_unused]] constexpr auto *kSplitPathTracerShaderGetStage = &SplitPathTracerShader::GetStage;
[[maybe_unused]] constexpr auto *kSplitPathTracerShaderGetInfo = &SplitPathTracerShader::GetShaderInfo;
} // namespace

ReblurSignalGenerator::ReblurSignalGenerator(const RenderConfig &render_config, RHIContext *rhi, Vector2UInt image_size,
                                             RHIImage *scene_texture, RHIImage *performance_sample_stats, RHITLAS *tlas)
    : render_config_(render_config), rhi_(rhi), image_size_(std::move(image_size)), scene_texture_(scene_texture),
      performance_sample_stats_(performance_sample_stats), tlas_(tlas)
{
    CreateResources();
}

ReblurSignalGenerator::~ReblurSignalGenerator() = default;

void ReblurSignalGenerator::CreateResources()
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
}

void ReblurSignalGenerator::BindBindlessResources(const BindlessManager &bindless_manager)
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

void ReblurSignalGenerator::BindSkyLight(const SkyRenderProxy *sky_light)
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

void ReblurSignalGenerator::BindTlas(RHITLAS *tlas, bool force_rebind)
{
    tlas_ = tlas;
    split_pt_pipeline_->GetShaderResource<SplitPathTracerShader>()->tlas().BindResource(tlas_, force_rebind);
}

void ReblurSignalGenerator::ClearPathTracingAccumulation()
{
    pt_clear_pass_->Render();
}

void ReblurSignalGenerator::PrepareForPathTracing()
{
    auto transition_aux_to_write = [](RHIImage *image) {
        image->Transition({.target_layout = RHIImageLayout::StorageWrite,
                           .after_stage = RHIPipelineStage::Top,
                           .before_stage = RHIPipelineStage::ComputeShader});
    };

    transition_aux_to_write(diffuse_signal_.get());
    transition_aux_to_write(specular_signal_.get());
    transition_aux_to_write(normal_roughness_.get());
    transition_aux_to_write(view_z_.get());
    transition_aux_to_write(motion_vectors_.get());
    transition_aux_to_write(albedo_metallic_.get());
    transition_aux_to_write(pt_accumulation_.get());
}

void ReblurSignalGenerator::UpdateFrameData(const CameraRenderProxy &camera, const SkyRenderProxy *sky_light,
                                            const DirectionalLightRenderProxy *dir_light,
                                            const ReblurPathTracingParameters &parameters)
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
}

const RHIResourceRef<RHIPipelineState> &ReblurSignalGenerator::GetPathTracingPipeline() const
{
    return split_pt_pipeline_;
}

RHIImage *ReblurSignalGenerator::GetDiffuseSignal() const
{
    return diffuse_signal_.get();
}

RHIImage *ReblurSignalGenerator::GetSpecularSignal() const
{
    return specular_signal_.get();
}

RHIImage *ReblurSignalGenerator::GetNormalRoughness() const
{
    return normal_roughness_.get();
}

RHIImage *ReblurSignalGenerator::GetViewZ() const
{
    return view_z_.get();
}

RHIImage *ReblurSignalGenerator::GetMotionVectors() const
{
    return motion_vectors_.get();
}

RHIImage *ReblurSignalGenerator::GetAlbedoMetallic() const
{
    return albedo_metallic_.get();
}

RHIImage *ReblurSignalGenerator::GetPathTracingAccumulation() const
{
    return pt_accumulation_.get();
}
} // namespace sparkle
