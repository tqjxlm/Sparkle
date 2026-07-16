#if FRAMEWORK_APPLE

#include "MetalRenderTarget.h"

namespace sparkle
{
MetalRenderTarget::MetalRenderTarget(const Attribute &attribute, const RHIResourceRef<RHIImage> &color_image,
                                     const RHIResourceRef<RHIImage> &depth_image, const std::string &name)
    : RHIRenderTarget(attribute, depth_image, name)
{
    SetColorImage(color_image, 0);
}

MetalRenderTarget::MetalRenderTarget(const Attribute &attribute, const RHIRenderTarget::ColorImageArray &color_images,
                                     const RHIResourceRef<RHIImage> &depth_image, const std::string &name)
    : RHIRenderTarget(attribute, color_images, depth_image, name)
{
}
} // namespace sparkle

#endif
