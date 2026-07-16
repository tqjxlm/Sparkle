#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

namespace sparkle
{
class MetalRenderTarget : public RHIRenderTarget
{
public:
    MetalRenderTarget(const Attribute &attribute, const RHIResourceRef<RHIImage> &color_image,
                      const RHIResourceRef<RHIImage> &depth_image, const std::string &name);

    MetalRenderTarget(const Attribute &attribute, const RHIRenderTarget::ColorImageArray &color_images,
                      const RHIResourceRef<RHIImage> &depth_image, const std::string &name);
};
} // namespace sparkle

#endif
