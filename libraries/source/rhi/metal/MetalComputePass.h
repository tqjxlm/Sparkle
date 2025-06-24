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

    void Begin();

    void End();

    [[nodiscard]] id<MTLComputeCommandEncoder> GetEncoder() const
    {
        return compute_encoder_;
    }

private:
    id<MTLComputeCommandEncoder> compute_encoder_;

    id<MTLCounterSampleBuffer> counter_sample_buffer_;

    MTLComputePassDescriptor *descriptor_;
};
} // namespace sparkle

#endif
