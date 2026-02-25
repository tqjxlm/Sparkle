#include "renderer/denoiser/ReblurDenoiser.h"

#include "rhi/RHI.h"

namespace sparkle
{

class ReblurClassifyTilesShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurClassifyTilesShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/reblur_classify_tiles.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(inViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(pointSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(outTiles, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        float denoising_range;
    };
};

ReblurDenoiser::ReblurDenoiser(RHIContext *rhi, uint32_t width, uint32_t height)
    : rhi_(rhi), width_(width), height_(height)
{
    CreateTextures();
    CreatePipelines();
}

ReblurDenoiser::~ReblurDenoiser() = default;

void ReblurDenoiser::CreateTextures()
{
    auto make_image = [this](PixelFormat format, uint32_t w, uint32_t h, const std::string &name) {
        return rhi_->CreateImage(
            RHIImage::Attribute{
                .format = format,
                .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                            .filtering_method_min = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
                .width = w,
                .height = h,
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV |
                          RHIImage::ImageUsage::TransferDst,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
            },
            name);
    };

    denoised_diffuse_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurDenoisedDiffuse");
    denoised_specular_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurDenoisedSpecular");

    uint32_t tile_w = (width_ + 15) / 16;
    uint32_t tile_h = (height_ + 15) / 16;
    tiles_ = make_image(PixelFormat::R32_FLOAT, tile_w, tile_h, "ReblurTiles");
}

void ReblurDenoiser::CreatePipelines()
{
    compute_pass_ = rhi_->CreateComputePass("ReblurComputePass", false);

    // ClassifyTiles pipeline
    classify_tiles_shader_ = rhi_->CreateShader<ReblurClassifyTilesShader>();
    classify_tiles_pipeline_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurClassifyTilesPipeline");
    classify_tiles_pipeline_->SetShader<RHIShaderStage::Compute>(classify_tiles_shader_);
    classify_tiles_pipeline_->Compile();

    classify_tiles_ub_ =
        rhi_->CreateBuffer({.size = sizeof(ReblurClassifyTilesShader::UniformBufferData),
                            .usages = RHIBuffer::BufferUsage::UniformBuffer,
                            .mem_properties = RHIMemoryProperty::None,
                            .is_dynamic = true},
                           "ReblurClassifyTilesUBO");

    auto *resources = classify_tiles_pipeline_->GetShaderResource<ReblurClassifyTilesShader>();
    resources->ubo().BindResource(classify_tiles_ub_);
    resources->outTiles().BindResource(tiles_->GetDefaultView(rhi_));
}

void ReblurDenoiser::Denoise(const ReblurInputBuffers &inputs, const ReblurSettings &settings,
                             const ReblurMatrices & /*matrices*/, uint32_t /*frame_index*/)
{
    ClassifyTiles(inputs, settings);

    // Passthrough: copy input signals directly to denoised output
    inputs.diffuse_radiance_hit_dist->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                                   .after_stage = RHIPipelineStage::ComputeShader,
                                                   .before_stage = RHIPipelineStage::Transfer});
    denoised_diffuse_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                   .after_stage = RHIPipelineStage::Top,
                                   .before_stage = RHIPipelineStage::Transfer});
    inputs.diffuse_radiance_hit_dist->CopyToImage(denoised_diffuse_.get());

    inputs.specular_radiance_hit_dist->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                                    .after_stage = RHIPipelineStage::ComputeShader,
                                                    .before_stage = RHIPipelineStage::Transfer});
    denoised_specular_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                    .after_stage = RHIPipelineStage::Top,
                                    .before_stage = RHIPipelineStage::Transfer});
    inputs.specular_radiance_hit_dist->CopyToImage(denoised_specular_.get());

    internal_frame_index_++;
}

void ReblurDenoiser::ClassifyTiles(const ReblurInputBuffers &inputs, const ReblurSettings & /*settings*/)
{
    // Bind viewZ input (re-bind each frame in case the resource changes)
    auto *resources = classify_tiles_pipeline_->GetShaderResource<ReblurClassifyTilesShader>();
    resources->inViewZ().BindResource(inputs.view_z->GetDefaultView(rhi_));
    resources->pointSampler().BindResource(inputs.view_z->GetSampler());

    // Upload UBO
    ReblurClassifyTilesShader::UniformBufferData ubo{
        .resolution = {width_, height_},
        .denoising_range = 1000.f,
    };
    classify_tiles_ub_->Upload(rhi_, &ubo);

    // Transition tiles to StorageWrite
    tiles_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                        .after_stage = RHIPipelineStage::Top,
                        .before_stage = RHIPipelineStage::ComputeShader});

    // Dispatch with tile-granularity (one thread group per tile)
    uint32_t tile_w = (width_ + 15) / 16;
    uint32_t tile_h = (height_ + 15) / 16;

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(classify_tiles_pipeline_, {tile_w * 16, tile_h * 16, 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);

    // Transition tiles to Read for subsequent passes
    tiles_->Transition({.target_layout = RHIImageLayout::Read,
                        .after_stage = RHIPipelineStage::ComputeShader,
                        .before_stage = RHIPipelineStage::ComputeShader});
}

RHIImage *ReblurDenoiser::GetDenoisedDiffuse() const
{
    return denoised_diffuse_.get();
}

RHIImage *ReblurDenoiser::GetDenoisedSpecular() const
{
    return denoised_specular_.get();
}

void ReblurDenoiser::Reset()
{
    internal_frame_index_ = 0;
    history_valid_ = false;
}
} // namespace sparkle
