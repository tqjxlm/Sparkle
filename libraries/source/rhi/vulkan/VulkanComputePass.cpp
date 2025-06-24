#if ENABLE_VULKAN

#include "VulkanComputePass.h"

#include "VulkanContext.h"
#include "rhi/RHI.h"

namespace sparkle
{
VulkanComputePass::VulkanComputePass(RHIContext *rhi, bool need_timestamp, const std::string &name)
    : RHIComputePass(rhi, need_timestamp, name)
{
    if (need_timestamp_)
    {
        for (auto i = 0u; i < rhi->GetMaxFramesInFlight(); i++)
        {
            timers_.push_back(rhi->CreateTimer("ComputePassTimer"));
        }
    }
}

void VulkanComputePass::Begin()
{
    if (need_timestamp_)
    {
        auto frame_index = context->GetRHI()->GetFrameIndex();
        if (timers_[frame_index]->GetStatus() != RHITimer::Status::Inactive)
        {
            execution_time_ms_[frame_index] = timers_[frame_index]->GetTime();
        }

        timers_[frame_index]->Begin();
    }
}

void VulkanComputePass::End()
{
    if (need_timestamp_)
    {
        auto frame_index = context->GetRHI()->GetFrameIndex();
        timers_[frame_index]->End();
    }
}
} // namespace sparkle

#endif
