#include "rhi/RHIImage.h"

#include "rhi/RHI.h"

namespace sparkle
{
std::vector<char> RHIImage::ReadToMemory(RHIContext *rhi)
{
    auto image_size = GetStorageSize();

    auto staging_buffer =
        rhi->CreateBuffer({.size = image_size,
                           .usages = RHIBuffer::BufferUsage::TransferDst,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "ImageReadBackStagingBuffer");

    rhi->BeginCommandBuffer();

    Transition({.target_layout = RHIImageLayout::TransferSrc,
                .after_stage = RHIPipelineStage::Bottom,
                .before_stage = RHIPipelineStage::Transfer});
    CopyToBuffer(staging_buffer.get());
    Transition({.target_layout = RHIImageLayout::Read,
                .after_stage = RHIPipelineStage::Transfer,
                .before_stage = RHIPipelineStage::PixelShader});

    rhi->SubmitCommandBuffer();

    rhi->WaitForDeviceIdle();

    const char *buffer_data = reinterpret_cast<const char *>(staging_buffer->Lock());

    std::vector<char> data(buffer_data, buffer_data + image_size);

    staging_buffer->UnLock();

    return data;
}

RHIImage::RHIImage(const Attribute &attributes, const std::string &name) : RHIResource(name), attributes_(attributes)
{
    if (attributes_.usages & ImageUsage::Texture)
    {
        ASSERT(attributes_.sampler.address_mode != RHISampler::SamplerAddressMode::Count);
    }

    for (auto i = 0u; i < attributes.mip_levels; i++)
    {
        current_layout_[i] = RHIImageLayout::Undefined;
    }
}

RHIImageView::RHIImageView(Attribute attribute, RHIImage *image)
    : RHIResource(image->GetName()), attribute_(std::move(attribute)), image_(image)
{
}

RHIResourceRef<RHIImageView> RHIImage::GetView(RHIContext *rhi, const RHIImageView::Attribute &attribute)
{
    auto found = image_views_.find(attribute);
    if (found != image_views_.end())
    {
        return found->second;
    }

    auto view = rhi->CreateImageView(this, attribute);

    image_views_.emplace(attribute, view);

    return view;
}

RHIResourceRef<RHIImageView> RHIImage::GetDefaultView(RHIContext *rhi)
{
    switch (attributes_.type)
    {
    case ImageType::Image2D:
        return GetView(rhi, {
                                .type = RHIImageView::ImageViewType::Image2D,
                                .mip_level_count = attributes_.mip_levels,
                            });
    case ImageType::Image2DCube:
        return GetView(rhi, {
                                .type = RHIImageView::ImageViewType::Image2DCube,
                                .mip_level_count = attributes_.mip_levels,
                                .array_layer_count = 6,
                            });
    default:
        UnImplemented(attributes_.type);
        return nullptr;
    }
}
} // namespace sparkle
