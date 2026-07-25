#include "renderer/denoiser/DenoiserHandoff.h"

#include <algorithm>

namespace sparkle
{
DenoiserHandoff::DenoiserHandoff(uint32_t max_sample_per_pixel)
{
    applies_ = static_cast<float>(max_sample_per_pixel) >= StartSamples;
    end_ = applies_ ? std::min(EndSamples, static_cast<float>(max_sample_per_pixel)) : EndSamples;
    start_ = std::min(StartSamples, 0.25f * end_);
}

float DenoiserHandoff::ComputeWeight(float accumulated_samples, bool final_frame) const
{
    if (!applies_)
    {
        return 0.f;
    }
    if (final_frame)
    {
        return 1.f;
    }
    return std::clamp((accumulated_samples - start_) / (end_ - start_), 0.f, 1.f);
}
} // namespace sparkle
