#pragma once

#include "renderer/debug/PerformanceMetrics.h"
#include "rhi/RHIImage.h"

#include <memory>
#include <mutex>
#include <vector>

namespace sparkle
{
class RHIContext;
class RHIShader;
class RHIPipelineState;
class RHIComputePass;
class RHIBuffer;

class ConvergenceMeasurePass
{
public:
    ConvergenceMeasurePass(RHIContext *rhi, uint32_t width, uint32_t height);

    void InitRenderResources();

    void Reset();

    void Measure(RHIImage *sample_stats_image, RHIPipelineStage sample_stats_after_stage, uint32_t sample_count,
                 uint32_t batch_sample_count);

    [[nodiscard]] PerformanceMetrics GetLatestMetrics() const;

private:
    static constexpr uint32_t ThreadGroupSizeX = 16;
    static constexpr uint32_t ThreadGroupSizeY = 16;

    struct TileMetrics
    {
        float sum_variance = 0.f;
        float max_variance = 0.f;
        uint32_t active_pixels = 0;
        uint32_t padding = 0;
    };

    static_assert(sizeof(TileMetrics) == 16);

    struct HistoryEntry
    {
        uint32_t sample_count = 0;
        float mean_luma_variance = 0.f;
        float max_luma_variance = 0.f;
    };

    struct AsyncState
    {
        mutable std::mutex mutex;
        PerformanceMetrics latest_metrics;
        std::vector<HistoryEntry> history;
        uint64_t generation = 0;
    };

    void UpdateLatestMetrics(const PerformanceMetrics &metrics) const;
    void InvalidateLatestMetrics(uint32_t sample_count, uint32_t frame_count) const;

    RHIContext *rhi_ = nullptr;
    Vector2UInt image_size_;
    Vector2UInt tile_count_;

    RHIResourceRef<RHIShader> compute_shader_;
    RHIResourceRef<RHIPipelineState> pipeline_state_;
    RHIResourceRef<RHIComputePass> compute_pass_;
    RHIResourceRef<RHIBuffer> uniform_buffer_;
    std::vector<RHIResourceRef<RHIBuffer>> tile_metric_buffers_;
    RHIResourceRef<RHIImage> variance_state_images_[2];
    uint32_t variance_state_index_ = 0;
    uint32_t measurement_count_ = 0;
    uint32_t accumulated_sample_count_ = 0;
    bool variance_state_initialized_ = false;

    std::shared_ptr<AsyncState> async_state_ = std::make_shared<AsyncState>();
};
} // namespace sparkle
