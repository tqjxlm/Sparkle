#include "renderer/resource/GBuffer.h"

#include "rhi/RHI.h"

namespace sparkle
{
void GBuffer::InitRenderResources(RHIContext *rhi, const Vector2UInt &image_size)
{
    RHIImage::Attribute gbuffer_attribute{
        .format = PixelFormat::RGBAUInt32,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
        .width = image_size.x(),
        .height = image_size.y(),
        .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::ColorAttachment,
        .msaa_samples = 1,
    };

    packed_texture = rhi->CreateImage(gbuffer_attribute, "GBufferPackedTexture");

    images[0] = packed_texture;
}

void GBuffer::Transition(const RHIImage::TransitionRequest &request) const
{
    for (const auto &image : images)
    {
        if (image)
        {
            image->Transition(request);
        }
    }
}
} // namespace sparkle
