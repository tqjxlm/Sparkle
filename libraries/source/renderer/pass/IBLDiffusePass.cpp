#include "renderer/pass/IBLDiffusePass.h"

#include "renderer/graph/RenderGraph.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/IblSettings.h"
#include "rhi/RHI.h"

#include <algorithm>

namespace sparkle
{
class IBLDiffuseMapComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(IBLDiffuseMapComputeShader, RHIShaderStage::Compute, "shaders/screen/ibl_diffuse.cs.slang",
                     "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(env_map, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(env_map_sampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(out_cube_map, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2Int resolution;
        uint32_t max_sample;
        uint32_t time_seed;
        float max_brightness;
        uint32_t sample_batch;
    };
};

IBLDiffusePass::~IBLDiffusePass() = default;

RHIResourceRef<RHIImage> IBLDiffusePass::CreateIBLMap(bool for_cooking, bool allow_write, PixelFormat resource_format)
{
    RHIImage::Attribute output_attribute;
    output_attribute.width =
        static_cast<uint32_t>(IblSettings::DiffuseMapSize * (static_cast<float>(env_map_->GetAttributes().width) /
                                                             static_cast<float>(env_map_->GetAttributes().height)));
    output_attribute.height = IblSettings::DiffuseMapSize;

    if (for_cooking)
    {
        output_attribute.format = PixelFormat::RGBAFloat;
        output_attribute.usages =
            RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::UAV | RHIImage::ImageUsage::TransferSrc;
    }
    else
    {
        output_attribute.format = resource_format;
        output_attribute.usages = IsCompressedFormat(resource_format)
                                      ? (RHIImage::ImageUsage::TransferDst | RHIImage::ImageUsage::Texture)
                                      : (RHIImage::ImageUsage::TransferDst | RHIImage::ImageUsage::Texture |
                                         RHIImage::ImageUsage::TransferSrc);
    }

    if (allow_write)
    {
        output_attribute.usages |= RHIImage::ImageUsage::UAV;
    }

    output_attribute.type = RHIImage::ImageType::Image2DCube;

    return rhi_->CreateImage(output_attribute, env_map_->GetName() + "_diffuse");
}

void IBLDiffusePass::InitRenderResources(const RenderConfig &)
{
    PrepareForCooking();

    compute_shader_ = rhi_->CreateShader<IBLDiffuseMapComputeShader>();

    pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "IBLDiffusePineline");
    pipeline_state_->SetShader<RHIShaderStage::Compute>(compute_shader_);

    pipeline_state_->Compile();

    cs_ub_ = rhi_->CreateBuffer({.size = sizeof(IBLDiffuseMapComputeShader::UniformBufferData),
                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                 .mem_properties = RHIMemoryProperty::None,
                                 .is_dynamic = true},
                                "IBLDiffusePSUBO");

    auto *shader_resource = pipeline_state_->GetShaderResource<IBLDiffuseMapComputeShader>();
    shader_resource->ubo().BindResource(cs_ub_);

    compute_pass_ = rhi_->CreateComputePass("IBLDiffuseComputePass", true);
}

void IBLDiffusePass::AddTo(RenderGraph &graph, unsigned samples_per_dispatch)
{
    ASSERT(!IsReady());

    const auto env_map = graph.Import("SkyMap", env_map_);
    const auto map = ImportCookingMap(graph, "IblDiffuseCook");

    const auto remaining_samples = target_sample_count_ - sample_count_;
    const uint32_t batch_size = std::min(std::max(samples_per_dispatch, 1u), remaining_samples);

    IBLDiffuseMapComputeShader::UniformBufferData ubo{
        .resolution = Vector2Int(ibl_image_->GetWidth(0), ibl_image_->GetHeight(0)),
        .max_sample = target_sample_count_,
        .time_seed = sample_count_ + 1u,
        .max_brightness = IblSettings::MaxEnvironmentBrightness,
        .sample_batch = batch_size,
    };
    cs_ub_->Upload(rhi_, &ubo);

    graph.AddComputePass("CookIblDiffuse", compute_pass_, [this, env_map, map](RGBuilder &builder) {
        using Table = IBLDiffuseMapComputeShader::ResourceTable;
        builder.Sampled(env_map, &Table::env_map, &Table::env_map_sampler, SkyRenderProxy::SkyMapSampler);
        builder.StorageReadWrite(map, &Table::out_cube_map);
        return [this, threads = Vector3UInt(ibl_image_->GetWidth(), ibl_image_->GetHeight(), 6u)](
                   RGComputeContext &context) { context.DispatchCompute(pipeline_state_, threads, {16u, 16u, 1u}); };
    });

    sample_count_ += batch_size;

    if (sample_count_ == target_sample_count_)
    {
        Complete();
        Logger::LogToScreen("IBLDiffuse", "");
    }
    else
    {
        float progress = static_cast<float>(sample_count_) / static_cast<float>(target_sample_count_) * 100.f;
        Logger::LogToScreen("IBLDiffuse", std::format("Caching ibl diffuse: {:.1f}%", progress));
    }
}

} // namespace sparkle
