#include "renderer/denoiser/ReblurDenoiser.h"

#include "core/math/Utilities.h"
#include "rhi/RHI.h"

namespace sparkle
{
namespace
{
class ReblurClassifyTilesShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurClassifyTilesShader, RHIShaderStage::Compute,
                     "shaders/denoiser/reblur/reblur_classify_tiles.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(in_view_z, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(out_tiles, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        Vector2 params;
    };
};

class ReblurPassthroughShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurPassthroughShader, RHIShaderStage::Compute, "shaders/utilities/reblur_passthrough.cs.slang",
                     "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(noisy_input, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(denoised_output, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
    };
};
} // namespace

ReblurDenoiser::ReblurDenoiser(RHIContext *rhi_context) : rhi_(rhi_context)
{
    ASSERT(rhi_);
}

void ReblurDenoiser::CreateTileMaskTexture()
{
    tile_resolution_ = Vector2UInt(utilities::DivideAndRoundUp(image_size_.x(), ReblurTileSize),
                                   utilities::DivideAndRoundUp(image_size_.y(), ReblurTileSize));

    tile_mask_texture_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = PixelFormat::R32_UINT,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = tile_resolution_.x(),
            .height = tile_resolution_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
            .memory_properties = RHIMemoryProperty::DeviceLocal,
            .mip_levels = 1,
            .msaa_samples = 1,
        },
        "ReblurTiles");
}

void ReblurDenoiser::Initialize(const Vector2UInt &image_size)
{
    ASSERT(image_size.x() > 0 && image_size.y() > 0);

    image_size_ = image_size;

    classify_tiles_shader_ = rhi_->CreateShader<ReblurClassifyTilesShader>();
    passthrough_shader_ = rhi_->CreateShader<ReblurPassthroughShader>();

    classify_tiles_pipeline_state_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurClassifyTilesPipeline");
    classify_tiles_pipeline_state_->SetShader<RHIShaderStage::Compute>(classify_tiles_shader_);
    classify_tiles_pipeline_state_->Compile();

    pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurPassthroughPipeline");
    pipeline_state_->SetShader<RHIShaderStage::Compute>(passthrough_shader_);
    pipeline_state_->Compile();

    classify_tiles_uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurClassifyTilesShader::UniformBufferData),
                                                         .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                         .mem_properties = RHIMemoryProperty::None,
                                                         .is_dynamic = true},
                                                        "ReblurClassifyTilesUniformBuffer");

    uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurPassthroughShader::UniformBufferData),
                                          .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                          .mem_properties = RHIMemoryProperty::None,
                                          .is_dynamic = true},
                                         "ReblurUniformBuffer");

    auto *classify_resources = classify_tiles_pipeline_state_->GetShaderResource<ReblurClassifyTilesShader>();
    classify_resources->ubo().BindResource(classify_tiles_uniform_buffer_);

    auto *cs_resources = pipeline_state_->GetShaderResource<ReblurPassthroughShader>();
    cs_resources->ubo().BindResource(uniform_buffer_);

    classify_tiles_compute_pass_ = rhi_->CreateComputePass("ReblurClassifyTilesComputePass", false);
    compute_pass_ = rhi_->CreateComputePass("ReblurComputePass", false);

    CreateTileMaskTexture();
}

void ReblurDenoiser::Resize(const Vector2UInt &image_size)
{
    ASSERT(image_size.x() > 0 && image_size.y() > 0);
    image_size_ = image_size;
    CreateTileMaskTexture();
}

void ReblurDenoiser::Dispatch(const FrontEndInputs &inputs, const RHIResourceRef<RHIImage> &denoised_output)
{
    ASSERT(inputs.noisy_input);
    ASSERT(inputs.normal_roughness);
    ASSERT(inputs.view_z);
    ASSERT(inputs.motion_vectors);
    ASSERT(inputs.diff_radiance_hitdist);
    ASSERT(inputs.spec_radiance_hitdist);
    ASSERT(denoised_output);
    ASSERT(classify_tiles_pipeline_state_);
    ASSERT(classify_tiles_compute_pass_);
    ASSERT(classify_tiles_uniform_buffer_);
    ASSERT(tile_mask_texture_);
    ASSERT(pipeline_state_);
    ASSERT(compute_pass_);
    ASSERT(uniform_buffer_);

    Vector2UInt output_size(denoised_output->GetWidth(), denoised_output->GetHeight());
    if (output_size.x() != image_size_.x() || output_size.y() != image_size_.y())
    {
        Resize(output_size);
    }

    ReblurClassifyTilesShader::UniformBufferData classify_ubo{
        .resolution = image_size_,
        .params = Vector2(denoising_range_, 0.0f),
    };
    classify_tiles_uniform_buffer_->Upload(rhi_, &classify_ubo);

    auto *classify_resources = classify_tiles_pipeline_state_->GetShaderResource<ReblurClassifyTilesShader>();
    classify_resources->in_view_z().BindResource(inputs.view_z->GetDefaultView(rhi_));
    classify_resources->out_tiles().BindResource(tile_mask_texture_->GetDefaultView(rhi_));

    tile_mask_texture_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                    .after_stage = RHIPipelineStage::Top,
                                    .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(classify_tiles_compute_pass_);
    rhi_->DispatchCompute(classify_tiles_pipeline_state_, {tile_resolution_.x() * 8u, tile_resolution_.y() * 4u, 1u},
                          {8u, 4u, 1u});
    rhi_->EndComputePass(classify_tiles_compute_pass_);

    tile_mask_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::ComputeShader});

    ReblurPassthroughShader::UniformBufferData ubo{.resolution = image_size_};
    uniform_buffer_->Upload(rhi_, &ubo);

    auto *cs_resources = pipeline_state_->GetShaderResource<ReblurPassthroughShader>();
    cs_resources->noisy_input().BindResource(inputs.noisy_input->GetDefaultView(rhi_));
    cs_resources->denoised_output().BindResource(denoised_output->GetDefaultView(rhi_));

    denoised_output->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                 .after_stage = RHIPipelineStage::Top,
                                 .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);

    denoised_output->Transition({.target_layout = RHIImageLayout::Read,
                                 .after_stage = RHIPipelineStage::ComputeShader,
                                 .before_stage = RHIPipelineStage::PixelShader});
}
} // namespace sparkle
