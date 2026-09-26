#include "rhi/RHIPass.h"

#include "rhi/RHI.h"

#include <limits>

namespace sparkle
{
RHIPass::RHIPass(RHIContext *rhi, bool need_timestamp, const std::string &name)
    : RHIResource(name), rhi_(rhi), execution_time_ms_(rhi->GetMaxFramesInFlight(), -1.f),
      read_frame_(rhi->GetMaxFramesInFlight(), std::numeric_limits<uint64_t>::max())
{
    if (need_timestamp && rhi->SupportsPassTimestamps())
    {
        for (auto i = 0u; i < rhi->GetMaxFramesInFlight(); i++)
        {
            timers_.push_back(rhi->CreateTimer(name));
        }
    }
}

RHITimer *RHIPass::SelectTimer()
{
    if (timers_.empty())
    {
        return nullptr;
    }

    const auto frame_index = rhi_->GetFrameIndex();
    auto &timer = timers_[frame_index];
    // a later begin in the same frame finds the timer waiting on this frame's own unsubmitted queries
    const auto frame = rhi_->GetRenderedFrameCount();
    if (read_frame_[frame_index] != frame)
    {
        read_frame_[frame_index] = frame;
        execution_time_ms_[frame_index] = timer->GetStatus() == RHITimer::Status::Ready ? timer->GetTime() : -1.f;
    }

    return timer.get();
}

void RHIPass::BeginTimer(RHICommandContext &command_context)
{
    active_timer_ = SelectTimer();
    if (active_timer_)
    {
        active_timer_->Begin(command_context);
    }
}

void RHIPass::EndTimer(RHICommandContext &command_context)
{
    if (active_timer_)
    {
        active_timer_->End(command_context);
        active_timer_ = nullptr;
    }
}
} // namespace sparkle
