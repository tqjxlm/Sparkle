#include "rhi/RHIRenderTarget.h"

#include "core/Logger.h"

namespace sparkle
{
RHIRenderTarget::RHIRenderTarget(const Attribute &attribute, const RHIResourceRef<RHIImage> &depth_image,
                                 const std::string &name)
    : RHIResource(name), attribute_(attribute), is_back_buffer_(true)
{
    ASSERT_F(attribute_.mip_level == 0, "Back buffer render target cannot have a miplevel {}", attribute_.mip_level);
    for (auto i = 0u; i < MaxNumColorImage; i++)
    {
        ASSERT_F(attribute_.color_attributes_[i].format == PixelFormat::Count,
                 "Back buffer render target cannot have any specified color attribute. Invalid slot {}", i);
    }

    SetDepthImage(depth_image);
}

RHIRenderTarget::RHIRenderTarget(const Attribute &attribute, const ColorImageArray &color_images,
                                 const RHIResourceRef<RHIImage> &depth_image, const std::string &name)
    : RHIResource(name), attribute_(attribute)
{
    for (auto i = 0u; i < color_images.size(); i++)
    {
        SetColorImage(color_images[i], i);
    }
    SetDepthImage(depth_image);
}

void RHIRenderTarget::Cleanup()
{
    Log(Debug, "Cleanup RT {}", GetName());

    std::ranges::fill(color_images_, nullptr);

    depth_image_ = nullptr;

    std::ranges::fill(msaa_images_, nullptr);
}
} // namespace sparkle
