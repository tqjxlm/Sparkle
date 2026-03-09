#pragma once

#include <cstdint>

namespace sparkle
{
struct PerformanceMetrics
{
    bool valid = false;
    uint32_t sample_count = 0;
    uint32_t frame_count = 0;
    uint32_t compared_pixels = 0;
    float mean_luma_variance = 0.f;
    float max_luma_variance = 0.f;
    float mean_variance_improvement = 0.f;
    float max_variance_improvement = 0.f;
    float mean_relative_variance_improvement = 0.f;
    float max_relative_variance_improvement = 0.f;
};
} // namespace sparkle
