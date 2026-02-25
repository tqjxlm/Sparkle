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
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
            },
            name);
    };

    denoised_diffuse_ = make_image(PixelFormat::RGBAFloat16, "ReblurDenoisedDiffuse");
    denoised_specular_ = make_image(PixelFormat::RGBAFloat16, "ReblurDenoisedSpecular");
}

void ReblurDenoiser::Denoise(const ReblurInputBuffers & /*inputs*/, const ReblurSettings & /*settings*/,
                             const ReblurMatrices & /*matrices*/, uint32_t /*frame_index*/)
{
    // TODO: implement pipeline stages
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
