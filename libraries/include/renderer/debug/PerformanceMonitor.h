#pragma once

#include "core/math/Types.h"
#include "renderer/debug/PerformanceMetrics.h"
#include "rhi/RHIImage.h"

#include <memory>

namespace sparkle
{
struct RenderConfig;
class RHIContext;
class ConvergenceMeasurePass;

class PerformanceMonitor
{
public:
    PerformanceMonitor(const RenderConfig &render_config, RHIContext *rhi, const Vector2UInt &image_size);

    ~PerformanceMonitor();

    void Reset();

    void Measure(RHIImage *sample_stats_image, RHIPipelineStage sample_stats_after_stage, uint32_t sample_count,
                 uint32_t batch_sample_count);

    void UpdateOverlay() const;

    [[nodiscard]] PerformanceMetrics GetLatestMetrics() const;

private:
    void EnsureMeasurePass();
    void UpdateStabilityState(const PerformanceMetrics &metrics) const;

    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] bool IsStable(const PerformanceMetrics &metrics) const;

    const RenderConfig &render_config_;
    RHIContext *rhi_ = nullptr;
    Vector2UInt image_size_;
    std::unique_ptr<ConvergenceMeasurePass> measure_pass_;
    mutable uint32_t stable_frame_streak_ = 0;
    mutable uint32_t last_stability_frame_count_ = 0;
};
} // namespace sparkle
