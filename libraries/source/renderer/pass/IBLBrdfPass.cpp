#include "renderer/pass/IBLBrdfPass.h"

#include "renderer/pass/ClearTexturePass.h"
#include "rhi/RHI.h"

#include <algorithm>

namespace sparkle
{
class IBLBrdfComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(IBLBrdfComputeShader, RHIShaderStage::Compute, "shaders/screen/ibl_brdf.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(out_texture, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2Int resolution;
        uint32_t time_seed;
        uint32_t max_sample;
        uint32_t sample_batch;
    };
};

IBLBrdfPass::IBLBrdfPass(RHIContext *ctx) : IBLPass(ctx, nullptr)
{
}

IBLBrdfPass::~IBLBrdfPass() = default;

RHIResourceRef<RHIImage> IBLBrdfPass::CreateIBLMap(bool for_cooking, bool allow_write)
{
    RHIImage::Attribute output_attribute;
    output_attribute.width = IblMapSize;
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

    output_attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                                .filtering_method_min = RHISampler::FilteringMethod::Linear,
                                .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                                .filtering_method_mipmap = RHISampler::FilteringMethod::Linear,
                                .enable_anisotropy = false};

    return rhi_->CreateImage(output_attribute, "ibl_brdf_map");
}

void IBLBrdfPass::InitRenderResources(const RenderConfig &config)
{
    TryLoad();

    if (IsReady())
    {
        return;
    }

    clear_target_ = rhi_->CreateRenderTarget({}, ibl_image_, nullptr, "IBLClearPassRenderTarget");

    clear_pass_ = PipelinePass::Create<ClearTexturePass>(config, rhi_, Vector4(0, 0, 0, 1),
                                                         RHIImageLayout::StorageWrite, clear_target_);

    compute_shader_ = rhi_->CreateShader<IBLBrdfComputeShader>();

    pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "IBLBrdfPineline");
    pipeline_state_->SetShader<RHIShaderStage::Compute>(compute_shader_);

    pipeline_state_->Compile();

    cs_ub_ = rhi_->CreateBuffer({.size = sizeof(IBLBrdfComputeShader::UniformBufferData),
                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                 .mem_properties = RHIMemoryProperty::None,
                                 .is_dynamic = true},
                                "IBLBrdfPSUBO");

    auto *shader_resource = pipeline_state_->GetShaderResource<IBLBrdfComputeShader>();
    shader_resource->ubo().BindResource(cs_ub_);

    shader_resource->out_texture().BindResource(ibl_image_->GetDefaultView(rhi_));

    compute_pass_ = rhi_->CreateComputePass("IBLBrdfComputePass", false);
}

void IBLBrdfPass::CookOnTheFly(const RenderConfig &, unsigned samples_per_dispatch)
{
    ASSERT(!IsReady());

    if (sample_count_ == 0)
    {
        clear_pass_->Render();
    }

    const auto remaining_samples = target_sample_count_ - sample_count_;
    const uint32_t batch_size = std::min(std::max(samples_per_dispatch, 1u), remaining_samples);

    IBLBrdfComputeShader::UniformBufferData ubo{
        .resolution = Vector2Int(ibl_image_->GetWidth(0), ibl_image_->GetHeight(0)),
        .time_seed = sample_count_ + 1u,
        .max_sample = target_sample_count_,
        .sample_batch = batch_size,
    };
    cs_ub_->Upload(rhi_, &ubo);

    Render();

    sample_count_ += batch_size;

    if (sample_count_ == target_sample_count_)
    {
        Complete();
        Logger::LogToScreen("IBLBrdf", "");
    }
    else
    {
        float progress = static_cast<float>(sample_count_) / static_cast<float>(target_sample_count_) * 100.f;
        Logger::LogToScreen("IBLBrdf", std::format("Caching ibl brdf: {:.1f}%", progress));
    }
}

void IBLBrdfPass::Render()
{
    rhi_->BeginComputePass(compute_pass_);

    rhi_->DispatchCompute(pipeline_state_, {ibl_image_->GetWidth(), ibl_image_->GetHeight(), 1u}, {16u, 16u, 1u});

    rhi_->EndComputePass(compute_pass_);
}

std::string IBLBrdfPass::GetCachePath() const
{
    return "cached/ibl/brdf.ibl";
}
} // namespace sparkle
