#include "renderer/denoiser/ReblurDenoiser.h"

#include "rhi/RHI.h"

namespace sparkle
{
ReblurDenoiser::ReblurDenoiser(RHIContext *rhi, uint32_t width, uint32_t height)
    : rhi_(rhi), width_(width), height_(height)
{
    CreateTextures();
}

ReblurDenoiser::~ReblurDenoiser() = default;

void ReblurDenoiser::CreateTextures()
{
    auto make_image = [this](PixelFormat format, const std::string &name) {
        return rhi_->CreateImage(
            RHIImage::Attribute{
                .format = format,
                .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                            .filtering_method_min = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
                .width = width_,
                .height = height_,
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV |
                          RHIImage::ImageUsage::TransferDst,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
            },
            name);
    };

    denoised_diffuse_ = make_image(PixelFormat::RGBAFloat16, "ReblurDenoisedDiffuse");
    denoised_specular_ = make_image(PixelFormat::RGBAFloat16, "ReblurDenoisedSpecular");
}

void ReblurDenoiser::Denoise(const ReblurInputBuffers &inputs, const ReblurSettings & /*settings*/,
                             const ReblurMatrices & /*matrices*/, uint32_t /*frame_index*/)
{
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
