#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

namespace sparkle
{
class MetalRenderPass : public RHIRenderPass
{
public:
    MetalRenderPass(const Attribute &attribute, const RHIResourceRef<RHIRenderTarget> &rt, const std::string &name)
        : RHIRenderPass(attribute, rt, name)
    {
    }

    void Begin();

    void End();

    id<MTLRenderCommandEncoder> GetRenderEncoder()
    {
        return render_encoder_;
    }

    [[nodiscard]] MTLRenderPassDescriptor *GetDescriptor() const;

private:
    id<MTLRenderCommandEncoder> render_encoder_;
};
} // namespace sparkle

#endif
