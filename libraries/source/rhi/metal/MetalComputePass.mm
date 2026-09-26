#if FRAMEWORK_APPLE

#include "MetalComputePass.h"

#include "MetalContext.h"
#include "MetalTimer.h"
#include "rhi/RHI.h"

namespace sparkle
{
MetalComputePass::MetalComputePass(RHIContext *rhi, bool need_timestamp, const std::string &name)
    : RHIComputePass(rhi, need_timestamp, name)
{
    descriptor_ = [[MTLComputePassDescriptor alloc] init];
    descriptor_.dispatchType = MTLDispatchTypeSerial;

    if (need_timestamp)
    {
        for (auto i = 0u; i < rhi->GetMaxFramesInFlight(); i++)
        {
            timers_.push_back(rhi->CreateTimer(name));
        }
    }
}

id<MTLComputeCommandEncoder> MetalComputePass::Begin(id<MTLCommandBuffer> command_buffer)
{
    if (need_timestamp_)
    {
        auto frame_index = context->GetRHI()->GetFrameIndex();
        auto &timer = timers_[frame_index];
        if (timer->GetStatus() != RHITimer::Status::Inactive)
        {
            execution_time_ms_[frame_index] = timer->GetTime();
        }

        RHICast<MetalTimer>(timer)->AttachTo(descriptor_);
        timer->Begin();
    }

    id<MTLComputeCommandEncoder> compute_encoder = [command_buffer computeCommandEncoderWithDescriptor:descriptor_];
    ASSERT(compute_encoder);

    SetDebugInfo(compute_encoder, GetName());

    return compute_encoder;
}

void MetalComputePass::End()
{
    if (need_timestamp_)
    {
        timers_[context->GetRHI()->GetFrameIndex()]->End();
    }
}

} // namespace sparkle

#endif
