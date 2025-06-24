#include "renderer/pass/IBLDiffusePass.h"

#include "renderer/pass/ClearTexturePass.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
class IBLDiffuseMapComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(IBLDiffuseMapComputeShader, RHIShaderStage::Compute, "shaders/screen/ibl_diffuse.cs.hlsl", "main")

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
    };
};

IBLDiffusePass::~IBLDiffusePass() = default;

RHIResourceRef<RHIImage> IBLDiffusePass::CreateIBLMap(bool for_cooking, bool allow_write)
{
    RHIImage::Attribute output_attribute;
    output_attribute.width = static_cast<uint32_t>(IblMapSize * (static_cast<float>(env_map_->GetAttributes().width) /
                                                                 static_cast<float>(env_map_->GetAttributes().height)));
    output_attribute.height = IblMapSize;

    if (for_cooking)
    {
        output_attribute.format = PixelFormat::RGBAFloat;
        output_attribute.usages =
            RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::UAV | RHIImage::ImageUsage::TransferSrc;
    }
    else
    {
        output_attribute.format = PixelFormat::RGBAFloat16;
        output_attribute.usages =
            RHIImage::ImageUsage::TransferDst | RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::TransferSrc;
    }

    if (allow_write)
    {
        output_attribute.usages |= RHIImage::ImageUsage::UAV;
    }

    output_attribute.type = RHIImage::ImageType::Image2DCube;

    output_attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                                .filtering_method_min = RHISampler::FilteringMethod::Linear,
                                .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                                .filtering_method_mipmap = RHISampler::FilteringMethod::Linear};

    return rhi_->CreateImage(output_attribute, env_map_->GetName() + "_diffuse");
}

void IBLDiffusePass::InitRenderResources(const RenderConfig &)
{
    TryLoad();

    if (IsReady())
    {
        return;
    }

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
    shader_resource->env_map().BindResource(env_map_->GetDefaultView(rhi_));
    shader_resource->env_map_sampler().BindResource(env_map_->GetSampler());

    shader_resource->out_cube_map().BindResource(ibl_image_->GetView(
        rhi_, RHIImageView::Attribute{.type = RHIImageView::ImageViewType::Image2DArray, .array_layer_count = 6}));

    compute_pass_ = rhi_->CreateComputePass("IBLDiffuseComputePass", false);
}

void IBLDiffusePass::CookOnTheFly(const RenderConfig &config)
{
    ASSERT(!IsReady());

    if (sample_count_ == 0)
    {
        for (uint8_t face = 0u; face < 6; face++)
        {
            clear_target_ =
                rhi_->CreateRenderTarget({.array_layer = face}, ibl_image_, nullptr, "IBLClearPassRenderTarget");

            clear_pass_ = PipelinePass::Create<ClearTexturePass>(config, rhi_, Vector4(0, 0, 0, 1),
                                                                 RHIImageLayout::StorageWrite, clear_target_);
            clear_pass_->Render();
        }
    }

    sample_count_++;

    IBLDiffuseMapComputeShader::UniformBufferData ubo{
        .resolution = Vector2Int(ibl_image_->GetWidth(0), ibl_image_->GetHeight(0)),
        .max_sample = target_sample_count_,
        .time_seed = sample_count_,
        .max_brightness = SkyRenderProxy::MaxIBLBrightness,
    };
    cs_ub_->Upload(rhi_, &ubo);

    Render();

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

void IBLDiffusePass::Render()
{
    rhi_->BeginComputePass(compute_pass_);

    rhi_->DispatchCompute(pipeline_state_, {ibl_image_->GetWidth(), ibl_image_->GetHeight(), 6u}, {16u, 16u, 1u});

    rhi_->EndComputePass(compute_pass_);
}

std::string IBLDiffusePass::GetCachePath() const
{
    return "cached/ibl/" + env_map_->GetName() + "_diffuse.ibl";
}
} // namespace sparkle
