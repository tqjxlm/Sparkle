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

void MetalComputePass::Begin()
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

    compute_encoder_ = [context->GetCurrentCommandBuffer() computeCommandEncoderWithDescriptor:descriptor_];
    ASSERT(compute_encoder_);

    SetDebugInfo(compute_encoder_, GetName());
}

void MetalComputePass::End()
{
    [compute_encoder_ endEncoding];

    if (need_timestamp_)
    {
        timers_[context->GetRHI()->GetFrameIndex()]->End();
    }
}

} // namespace sparkle

#endif
