#if FRAMEWORK_APPLE

#include "MetalComputePass.h"

#include "MetalTimer.h"

namespace sparkle
{
MetalComputePass::MetalComputePass(RHIContext *rhi, bool need_timestamp, const std::string &name)
    : RHIComputePass(rhi, need_timestamp, name)
{
    descriptor_ = [[MTLComputePassDescriptor alloc] init];
    descriptor_.dispatchType = MTLDispatchTypeSerial;
}

id<MTLComputeCommandEncoder> MetalComputePass::Begin(id<MTLCommandBuffer> command_buffer)
{
    if (auto *timer = GetActiveTimer())
    {
        RHICast<MetalTimer>(timer)->AttachTo(descriptor_);
    }

    id<MTLComputeCommandEncoder> compute_encoder = [command_buffer computeCommandEncoderWithDescriptor:descriptor_];
    ASSERT(compute_encoder);

    SetDebugInfo(compute_encoder, GetName());

    return compute_encoder;
}

} // namespace sparkle

#endif
