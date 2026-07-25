#pragma once

#include "rhi/RHIComputePass.h"
#include "rhi/RHIResource.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace sparkle
{
class RHIContext;

// Averages the GPU time of a set of compute passes and logs the means periodically. A frame
// slot's GPU time belongs to the previous submission that used that slot, so each frame harvests
// the slot's pending times before recording which stages it dispatches into the slot; a stage
// that skips a frame contributes nothing rather than a stale reading.
class PassTimingAggregator
{
public:
    PassTimingAggregator(RHIContext *rhi, std::string label, uint32_t max_frames_in_flight);

    // registration order is the log order, and the first stage drives the periodic log
    void AddStage(std::string name, RHIResourceRef<RHIComputePass> pass);

    // once per frame before dispatching, with each stage's will-run decision in registration
    // order. harvests the slot's pending times, then records the decisions
    void Sample(std::initializer_list<bool> will_run);

    // whether any stage recorded a timed frame
    [[nodiscard]] bool HasSamples() const;

    void LogTimings(const char *tag) const;

private:
    struct Stage
    {
        std::string name;
        RHIResourceRef<RHIComputePass> pass;
        double sum_ms = 0.0;
        uint32_t count = 0;
        // one flag per frame slot: whether the submission that last used the slot ran this stage
        std::vector<uint8_t> slot_ran;
    };

    RHIContext *rhi_;
    std::string label_;
    uint32_t max_frames_in_flight_;
    std::vector<Stage> stages_;
    uint32_t last_log_count_ = 0;
};
} // namespace sparkle
