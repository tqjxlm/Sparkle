#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

namespace sparkle
{
class MetalRenderPass : public RHIRenderPass
{
public:
    MetalRenderPass(RHIContext *rhi, const Attribute &attribute, const RHIResourceRef<RHIRenderTarget> &rt,
                    const std::string &name);

    // opens the pass's render encoder on the command buffer
    [[nodiscard]] id<MTLRenderCommandEncoder> Begin(id<MTLCommandBuffer> command_buffer) const;

    [[nodiscard]] MTLRenderPassDescriptor *GetDescriptor() const;

private:
    // lowers an RHIRenderingInfo into descriptor_
    void FillDescriptor(const RHIRenderingInfo &info) const;

    MTLRenderPassDescriptor *descriptor_;
};
} // namespace sparkle

#endif
