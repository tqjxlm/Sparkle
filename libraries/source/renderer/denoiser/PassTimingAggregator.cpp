#include "renderer/denoiser/PassTimingAggregator.h"

#include "core/Exception.h"
#include "core/Logger.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <format>
#include <utility>

namespace sparkle
{
namespace
{
constexpr uint32_t LogFrameInterval = 300;
} // namespace

PassTimingAggregator::PassTimingAggregator(RHIContext *rhi, std::string label, uint32_t max_frames_in_flight)
    : rhi_(rhi), label_(std::move(label)), max_frames_in_flight_(max_frames_in_flight)
{
}

void PassTimingAggregator::AddStage(std::string name, RHIResourceRef<RHIComputePass> pass)
{
    stages_.push_back(
        {.name = std::move(name), .pass = std::move(pass), .slot_ran = std::vector<uint8_t>(max_frames_in_flight_, 0)});
}

void PassTimingAggregator::Sample(std::initializer_list<bool> will_run)
{
    ASSERT_EQUAL(will_run.size(), stages_.size());

    const auto slot = rhi_->GetFrameIndex();

    const auto *decision = will_run.begin();
    for (auto &stage : stages_)
    {
        if (stage.slot_ran[slot] != 0)
        {
            const float time_ms = stage.pass->GetExecutionTime(slot);
            if (time_ms >= 0.f)
            {
                stage.sum_ms += static_cast<double>(time_ms);
                stage.count++;
            }
        }
        stage.slot_ran[slot] = *decision++ ? 1 : 0;
    }

    const uint32_t driver_count = stages_.empty() ? 0 : stages_.front().count;
    if (driver_count != last_log_count_ && driver_count % LogFrameInterval == 0)
    {
        last_log_count_ = driver_count;
        LogTimings("running");
    }
}

bool PassTimingAggregator::HasSamples() const
{
    return std::ranges::any_of(stages_, [](const Stage &stage) { return stage.count > 0; });
}

void PassTimingAggregator::LogTimings(const char *tag) const
{
    std::string means;
    for (const auto &stage : stages_)
    {
        // a stage with no timed frame reports -1 rather than a mean of nothing
        const double mean = stage.count > 0 ? stage.sum_ms / stage.count : -1.0;
        means += std::format(" {} {:.3f} ms (n={})", stage.name, mean, stage.count);
    }

    Log(Info, "[{}] {}{}", label_, tag, means);
}
} // namespace sparkle
