#include "renderer/pass/IBLSpecularPass.h"

#include "renderer/pass/ClearTexturePass.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"

#include <algorithm>

namespace sparkle
{
class IBLSpecularMapComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(IBLSpecularMapComputeShader, RHIShaderStage::Compute, "shaders/screen/ibl_specular.cs.slang",
                     "main")

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
        float roughness;
        float max_brightness;
        uint32_t sample_batch;
    };
};

IBLSpecularPass::~IBLSpecularPass() = default;

void IBLSpecularPass::InitRenderResources(const RenderConfig &)
{
    TryLoad();

    if (IsReady())
    {
        return;
    }

    compute_shader_ = rhi_->CreateShader<IBLSpecularMapComputeShader>();

    pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "IBLSpecularPineline");
    pipeline_state_->SetShader<RHIShaderStage::Compute>(compute_shader_);

    pipeline_state_->Compile();

    cs_ub_ = rhi_->CreateBuffer({.size = sizeof(IBLSpecularMapComputeShader::UniformBufferData),
                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                 .mem_properties = RHIMemoryProperty::None,
                                 .is_dynamic = true},
                                "IBLSpecularPSUBO");

    auto *shader_resource = pipeline_state_->GetShaderResource<IBLSpecularMapComputeShader>();
    shader_resource->ubo().BindResource(cs_ub_);
    shader_resource->env_map().BindResource(env_map_->GetDefaultView(rhi_));
    shader_resource->env_map_sampler().BindResource(env_map_->GetSampler());

    StartCacheLevel(0);

    compute_pass_ = rhi_->CreateComputePass("IBLSpecularComputePass", false);
}

void IBLSpecularPass::CookOnTheFly(const RenderConfig &config, unsigned samples_per_dispatch)
{
    ASSERT(!IsReady());

    if (sample_count_ == 0 && current_caching_level_ == 0)
    {
        for (uint8_t level = 0u; level < MipLevelCount; level++)
        {
            for (uint8_t face = 0u; face < 6; face++)
            {
                clear_target_ = rhi_->CreateRenderTarget({.mip_level = level, .array_layer = face}, ibl_image_, nullptr,
                                                         "IBLClearPassRenderTarget");
                clear_pass_ = PipelinePass::Create<ClearTexturePass>(config, rhi_, Vector4(0, 0, 0, 1),
                                                                     RHIImageLayout::StorageWrite, clear_target_);
                clear_pass_->Render();
            }
        }
    }

    const auto remaining_samples = target_sample_count_ - sample_count_;
    const uint32_t batch_size = std::min(std::max(samples_per_dispatch, 1u), remaining_samples);

    IBLSpecularMapComputeShader::UniformBufferData ubo{
        .resolution =
            Vector2Int(ibl_image_->GetWidth(current_caching_level_), ibl_image_->GetHeight(current_caching_level_)),
        .max_sample = target_sample_count_,
        .time_seed = sample_count_ + 1u,
        .roughness = 1.f / (MipLevelCount - 1) * current_caching_level_,
        .max_brightness = SkyRenderProxy::MaxIBLBrightness,
        .sample_batch = batch_size,
    };
    cs_ub_->Upload(rhi_, &ubo);

    Render();

    sample_count_ += batch_size;

    if (sample_count_ == target_sample_count_)
    {
        Log(Info, "Finished caching ibl specular level {}", current_caching_level_);

        if (current_caching_level_ + 1 == MipLevelCount)
        {
            Complete();
            Logger::LogToScreen("IBLSpecular", "");
        }
        else
        {
            StartCacheLevel(current_caching_level_ + 1);
        }
    }
    else
    {
        float progress = static_cast<float>(sample_count_) / static_cast<float>(target_sample_count_) * 100.f;
        Logger::LogToScreen("IBLSpecular",
                            std::format("Caching ibl specular {}: {:.1f}%", current_caching_level_, progress));
    }
}

RHIResourceRef<RHIImage> IBLSpecularPass::CreateIBLMap(bool for_cooking, bool allow_write)
{
    RHIImage::Attribute output_attribute;
    output_attribute.width = static_cast<uint32_t>(IblMapSize * (static_cast<float>(env_map_->GetAttributes().width) /
                                                                 static_cast<float>(env_map_->GetAttributes().height)));
    output_attribute.height = IblMapSize;
    output_attribute.mip_levels = MipLevelCount;

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
                                .filtering_method_mipmap = RHISampler::FilteringMethod::Linear,
                                .max_lod = (MipLevelCount - 1)};

    return rhi_->CreateImage(output_attribute, env_map_->GetName() + "_specular");
}

std::string IBLSpecularPass::GetCachePath() const
{
    return "cached/ibl/" + env_map_->GetName() + "_specular.ibl";
}

void IBLSpecularPass::StartCacheLevel(uint8_t level)
{
    current_caching_level_ = level;
    sample_count_ = 0;

    auto *shader_resource = pipeline_state_->GetShaderResource<IBLSpecularMapComputeShader>();

    shader_resource->out_cube_map().BindResource(
        ibl_image_->GetView(rhi_, RHIImageView::Attribute{
                                      .type = RHIImageView::ImageViewType::Image2DArray,
                                      .base_mip_level = level,
                                      .array_layer_count = 6,
                                  }));
}

void IBLSpecularPass::Render()
{
    rhi_->BeginComputePass(compute_pass_);

    rhi_->DispatchCompute(
        pipeline_state_,
        {ibl_image_->GetWidth(current_caching_level_), ibl_image_->GetHeight(current_caching_level_), 6u},
        {16u, 16u, 1u});

    rhi_->EndComputePass(compute_pass_);
}
} // namespace sparkle
