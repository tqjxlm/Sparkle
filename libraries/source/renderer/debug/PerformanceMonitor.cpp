#include "renderer/debug/PerformanceMonitor.h"

#include "core/Logger.h"
#include "renderer/RenderConfig.h"
#include "renderer/pass/ConvergenceMeasurePass.h"

namespace sparkle
{
PerformanceMonitor::PerformanceMonitor(const RenderConfig &render_config, RHIContext *rhi,
                                       const Vector2UInt &image_size)
    : render_config_(render_config), rhi_(rhi), image_size_(image_size)
{
}

PerformanceMonitor::~PerformanceMonitor() = default;

void PerformanceMonitor::Reset()
{
    if (measure_pass_)
    {
        measure_pass_->Reset();
    }

    stable_frame_streak_ = 0;
    last_stability_frame_count_ = 0;
}

void PerformanceMonitor::Measure(RHIImage *sample_stats_image, RHIPipelineStage sample_stats_after_stage,
                                 uint32_t sample_count, uint32_t batch_sample_count)
{
    if (!IsEnabled())
    {
        Reset();
        return;
    }

    EnsureMeasurePass();
    measure_pass_->Measure(sample_stats_image, sample_stats_after_stage, sample_count, batch_sample_count);
}

void PerformanceMonitor::UpdateOverlay() const
{
    if (!IsEnabled())
    {
        Logger::LogToScreen("Performance", "");
        return;
    }

    auto metrics = GetLatestMetrics();
    if (!metrics.valid)
    {
        Logger::LogToScreen("Performance", "Conv: warming up");
        return;
    }

    const bool stable = IsStable(metrics);
    const char *stability = stable ? "stable" : "settling";
    Logger::LogToScreen("Performance",
                        fmt::format("Conv {} {}/{} f {} spp {} var {:.6f} improve {:.4f}/{:.4f}", stability,
                                    stable_frame_streak_, render_config_.gpu_convergence_stability_frames,
                                    metrics.frame_count, metrics.sample_count, metrics.mean_luma_variance,
                                    metrics.mean_relative_variance_improvement,
                                    metrics.max_relative_variance_improvement));
}

PerformanceMetrics PerformanceMonitor::GetLatestMetrics() const
{
    return measure_pass_ ? measure_pass_->GetLatestMetrics() : PerformanceMetrics{};
}

void PerformanceMonitor::EnsureMeasurePass()
{
    if (measure_pass_)
    {
        return;
    }

    measure_pass_ = std::make_unique<ConvergenceMeasurePass>(rhi_, image_size_.x(), image_size_.y());
    measure_pass_->InitRenderResources();
}

bool PerformanceMonitor::IsEnabled() const
{
    return render_config_.measure_gpu_convergence;
}

void PerformanceMonitor::UpdateStabilityState(const PerformanceMetrics &metrics) const
{
    if (!metrics.valid)
    {
        stable_frame_streak_ = 0;
        last_stability_frame_count_ = 0;
        return;
    }

    if (metrics.frame_count == last_stability_frame_count_)
    {
        return;
    }

    last_stability_frame_count_ = metrics.frame_count;

    if (metrics.mean_relative_variance_improvement <= render_config_.gpu_convergence_threshold)
    {
        ++stable_frame_streak_;
    }
    else
    {
        stable_frame_streak_ = 0;
    }
}

bool PerformanceMonitor::IsStable(const PerformanceMetrics &metrics) const
{
    UpdateStabilityState(metrics);
    return stable_frame_streak_ >= render_config_.gpu_convergence_stability_frames;
}
} // namespace sparkle
