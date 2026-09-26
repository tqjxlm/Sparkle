#pragma once

#if FRAMEWORK_APPLE

#include "rhi/RHIComputePass.h"

#include "MetalRHIInternal.h"

namespace sparkle
{
class MetalComputePass : public RHIComputePass
{
public:
    MetalComputePass(RHIContext *rhi, bool need_timestamp, const std::string &name);

    ~MetalComputePass() override = default;

    // opens the pass's compute encoder on the command buffer
    [[nodiscard]] id<MTLComputeCommandEncoder> Begin(id<MTLCommandBuffer> command_buffer);

private:
    MTLComputePassDescriptor *descriptor_;
};
} // namespace sparkle

#endif
