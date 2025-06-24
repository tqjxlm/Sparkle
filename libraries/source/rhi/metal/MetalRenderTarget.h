#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

namespace sparkle
{
class MetalRenderTarget : public RHIRenderTarget
{
public:
    MetalRenderTarget(const Attribute &attribute, const RHIResourceRef<RHIImage> &depth_image, const std::string &name)
        : RHIRenderTarget(attribute, depth_image, name)
    {
        Init();
    }

    MetalRenderTarget(const Attribute &attribute, const RHIRenderTarget::ColorImageArray &color_images,
                      const RHIResourceRef<RHIImage> &depth_image, const std::string &name)
        : RHIRenderTarget(attribute, color_images, depth_image, name)
    {
        Init();
    }

private:
    void Init();
};
} // namespace sparkle

#endif
