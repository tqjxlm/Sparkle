#include "application/AppFramework.h"
#include "application/TestCase.h"

#include "core/Logger.h"

#include <cmath>
#include <vector>

namespace sparkle
{
namespace
{
using Result = TestCase::Result;
constexpr uint32_t EarlyStabilityGuardSampleCount = 16;
} // namespace

/// Usage: --test_case gpu_convergence_debug --pipeline gpu --measure_gpu_convergence true
class GpuConvergenceDebugTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        constexpr const char *test_name = "GpuConvergenceDebugTest";

        if (!app.GetRenderConfig().IsRayTracingMode())
        {
            Log(Error, "{} requires --pipeline gpu", test_name);
            return Result::Fail;
        }

        if (!app.GetRenderConfig().measure_gpu_convergence)
        {
            Log(Error, "{} requires --measure_gpu_convergence true", test_name);
            return Result::Fail;
        }

        auto metrics = app.GetLatestPerformanceMetrics();
        if (!metrics.valid)
        {
            return Result::Pending;
        }

        const auto expected_pixels = app.GetRenderConfig().image_width * app.GetRenderConfig().image_height;
        const bool finite_metrics =
            std::isfinite(metrics.mean_luma_variance) && std::isfinite(metrics.max_luma_variance) &&
            std::isfinite(metrics.mean_variance_improvement) && std::isfinite(metrics.max_variance_improvement) &&
            std::isfinite(metrics.mean_relative_variance_improvement) &&
            std::isfinite(metrics.max_relative_variance_improvement);
        if (!finite_metrics)
        {
            Log(Error, "{} received non-finite metrics", test_name);
            return Result::Fail;
        }

        if (metrics.sample_count < 3 || metrics.frame_count < 3 || metrics.compared_pixels != expected_pixels)
        {
            Log(Error, "{} got invalid coverage: sample_count={} frame_count={} compared_pixels={} expected={}",
                test_name, metrics.sample_count, metrics.frame_count, metrics.compared_pixels, expected_pixels);
            return Result::Fail;
        }

        if (metrics.mean_luma_variance < 0.f || metrics.max_luma_variance < 0.f ||
            metrics.mean_variance_improvement < 0.f || metrics.max_variance_improvement < 0.f ||
            metrics.mean_relative_variance_improvement < 0.f || metrics.max_relative_variance_improvement < 0.f)
        {
            Log(Error,
                "{} got invalid variance values: compared_pixels={} mean_variance={} mean_relative_improvement={} "
                "max_relative_improvement={}",
                test_name, metrics.compared_pixels, metrics.mean_luma_variance,
                metrics.mean_relative_variance_improvement, metrics.max_relative_variance_improvement);
            return Result::Fail;
        }

        if (metrics.frame_count != last_frame_count_)
        {
            last_frame_count_ = metrics.frame_count;

            if (metrics.mean_relative_variance_improvement <= app.GetRenderConfig().gpu_convergence_threshold)
            {
                ++stable_frame_streak_;
            }
            else
            {
                stable_frame_streak_ = 0;
            }
        }

        const auto stability_frames =
            std::max(EarlyStabilityGuardSampleCount, app.GetRenderConfig().gpu_convergence_stability_frames);
        if (metrics.sample_count < stability_frames)
        {
            return Result::Pending;
        }

        if (stable_frame_streak_ >= app.GetRenderConfig().gpu_convergence_stability_frames)
        {
            Log(Error,
                "{} became stable too early: sample_count={} stable_frame_streak={} mean_relative_improvement={} "
                "threshold={} stability_frames={}",
                test_name, metrics.sample_count, stable_frame_streak_, metrics.mean_relative_variance_improvement,
                app.GetRenderConfig().gpu_convergence_threshold,
                app.GetRenderConfig().gpu_convergence_stability_frames);
            return Result::Fail;
        }

        return Result::Pass;
    }

private:
    uint32_t stable_frame_streak_ = 0;
    uint32_t last_frame_count_ = 0;
};

/// Usage: --test_case gpu_convergence_report --pipeline gpu --measure_gpu_convergence true
class GpuConvergenceReportTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (!app.GetRenderConfig().IsRayTracingMode())
        {
            Log(Error, "GpuConvergenceReportTest requires --pipeline gpu");
            return Result::Fail;
        }

        if (!app.GetRenderConfig().measure_gpu_convergence)
        {
            Log(Error, "GpuConvergenceReportTest requires --measure_gpu_convergence true");
            return Result::Fail;
        }

        const auto target_sample_count = app.GetRenderConfig().max_sample_per_pixel;
        auto metrics = app.GetLatestPerformanceMetrics();
        if (!metrics.valid || metrics.sample_count < target_sample_count)
        {
            return Result::Pending;
        }

        Log(Info,
            "GpuConvergenceReportTest: sample_count={} frame_count={} compared_pixels={} mean_luma_variance={} "
            "max_luma_variance={} "
            "mean_variance_improvement={} max_variance_improvement={} mean_relative_variance_improvement={} "
            "max_relative_variance_improvement={}",
            metrics.sample_count, metrics.frame_count, metrics.compared_pixels, metrics.mean_luma_variance,
            metrics.max_luma_variance, metrics.mean_variance_improvement, metrics.max_variance_improvement,
            metrics.mean_relative_variance_improvement, metrics.max_relative_variance_improvement);
        return Result::Pass;
    }
};

/// Usage: --test_case gpu_convergence_curve --pipeline gpu --measure_gpu_convergence true
class GpuConvergenceCurveTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (!app.GetRenderConfig().IsRayTracingMode())
        {
            Log(Error, "GpuConvergenceCurveTest requires --pipeline gpu");
            return Result::Fail;
        }

        if (!app.GetRenderConfig().measure_gpu_convergence)
        {
            Log(Error, "GpuConvergenceCurveTest requires --measure_gpu_convergence true");
            return Result::Fail;
        }

        const auto target_sample_count = app.GetRenderConfig().max_sample_per_pixel;
        auto metrics = app.GetLatestPerformanceMetrics();
        if (metrics.sample_count == 0 || metrics.frame_count == 0)
        {
            return Result::Pending;
        }

        if (history_.empty())
        {
            if (metrics.sample_count != 1 || metrics.frame_count != 1)
            {
                Log(Error,
                    "GpuConvergenceCurveTest expected first post-load sample/frame to be 1, got sample_count={} "
                    "frame_count={}",
                    metrics.sample_count, metrics.frame_count);
                return Result::Fail;
            }

            history_.push_back(metrics);
            return Result::Pending;
        }

        const auto &last_metrics = history_.back();
        if (metrics.frame_count == last_metrics.frame_count)
        {
            return MaybeFlushHistory(target_sample_count);
        }

        if (metrics.sample_count < last_metrics.sample_count || metrics.frame_count < last_metrics.frame_count)
        {
            Log(Error,
                "GpuConvergenceCurveTest observed performance metrics reset during capture: last_sample_count={} "
                "sample_count={} last_frame_count={} frame_count={}",
                last_metrics.sample_count, metrics.sample_count, last_metrics.frame_count, metrics.frame_count);
            return Result::Fail;
        }

        if (metrics.frame_count != last_metrics.frame_count + 1u ||
            metrics.sample_count != last_metrics.sample_count + 1u)
        {
            Log(Error,
                "GpuConvergenceCurveTest missed a frame during capture: last_sample_count={} sample_count={} "
                "last_frame_count={} frame_count={}",
                last_metrics.sample_count, metrics.sample_count, last_metrics.frame_count, metrics.frame_count);
            return Result::Fail;
        }

        history_.push_back(metrics);
        return MaybeFlushHistory(target_sample_count);
    }

private:
    Result MaybeFlushHistory(uint32_t target_sample_count)
    {
        if (history_.back().sample_count < target_sample_count)
        {
            return Result::Pending;
        }

        if (history_.size() != target_sample_count)
        {
            Log(Error, "GpuConvergenceCurveTest expected {} samples from 1..{}, got count={}", target_sample_count,
                target_sample_count, history_.size());
            return Result::Fail;
        }

        for (uint32_t sample_index = 0; sample_index < target_sample_count; ++sample_index)
        {
            const auto &metrics = history_[sample_index];
            const auto expected_sample_count = sample_index + 1u;
            if (metrics.sample_count != expected_sample_count || metrics.frame_count != expected_sample_count)
            {
                Log(Error,
                    "GpuConvergenceCurveTest expected sample/frame {} at index {}, got sample_count={} frame_count={}",
                    expected_sample_count, sample_index, metrics.sample_count, metrics.frame_count);
                return Result::Fail;
            }

            Log(Info,
                "GpuConvergenceCurveTest: sample_count={} frame_count={} valid={} mean_luma_variance={} "
                "max_luma_variance={}",
                metrics.sample_count, metrics.frame_count, metrics.valid ? 1u : 0u, metrics.mean_luma_variance,
                metrics.max_luma_variance);
        }

        return Result::Pass;
    }

    std::vector<PerformanceMetrics> history_;
};

static TestCaseRegistrar<GpuConvergenceDebugTest> gpu_convergence_debug_registrar("gpu_convergence_debug");
static TestCaseRegistrar<GpuConvergenceReportTest> gpu_convergence_report_registrar("gpu_convergence_report");
static TestCaseRegistrar<GpuConvergenceCurveTest> gpu_convergence_curve_registrar("gpu_convergence_curve");
} // namespace sparkle
