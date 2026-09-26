#pragma once

#include "rhi/RHIResource.h"
#include "rhi/RHITimer.h"

#include <cstdint>
#include <vector>

namespace sparkle
{
class RHICommandContext;

// common base of render and compute passes. a pass created with need_timestamp measures its GPU time with one timer
// per frame in flight, from before its opening barriers to after its closing ones. a slot's time is read at the pass's
// first begin in that slot per frame, so it belongs to the previous submission that used the slot and is -1 when that
// result is not available; a pass recorded several times in one frame reports its last run. without
// RHIContext::SupportsPassTimestamps() nothing is measured.
class RHIPass : public RHIResource
{
public:
    RHIPass(RHIContext *rhi, bool need_timestamp, const std::string &name);

    // GPU time in ms, -1 when the slot has no result
    [[nodiscard]] float GetExecutionTime(unsigned frame_index) const
    {
        return execution_time_ms_[frame_index];
    }

protected:
    // the timer measuring the open compute pass, null when the pass is not timed
    [[nodiscard]] RHITimer *GetActiveTimer() const
    {
        return active_timer_;
    }

private:
    friend class RHICommandContext;

    // this frame's timer, after reading the time it measured last; null when the pass is not timed
    [[nodiscard]] RHITimer *SelectTimer();

    void BeginTimer(RHICommandContext &command_context);

    void EndTimer(RHICommandContext &command_context);

    RHIContext *rhi_;
    std::vector<RHIResourceRef<RHITimer>> timers_;
    std::vector<float> execution_time_ms_;
    // the rendered frame count at which each slot's time was last read
    std::vector<uint64_t> read_frame_;
    RHITimer *active_timer_ = nullptr;
};
} // namespace sparkle
