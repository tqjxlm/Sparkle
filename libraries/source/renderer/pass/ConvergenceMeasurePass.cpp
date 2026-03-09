#include "renderer/pass/ConvergenceMeasurePass.h"

#include "rhi/RHI.h"

#include <algorithm>
#include <cmath>

namespace sparkle
{
class ConvergenceMeasureShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ConvergenceMeasureShader, RHIShaderStage::Compute,
                     "shaders/utilities/convergence_measure.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(currentSampleStats, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(previousVarianceState, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(nextVarianceState, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outTileMetrics, RHIShaderResourceReflection::ResourceType::StorageBuffer)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        uint32_t previous_sample_count;
        uint32_t current_batch_sample_count;
        uint32_t tile_count_x;
    };
};

ConvergenceMeasurePass::ConvergenceMeasurePass(RHIContext *rhi, uint32_t width, uint32_t height)
    : rhi_(rhi), image_size_{width, height},
      tile_count_{(width + ThreadGroupSizeX - 1) / ThreadGroupSizeX, (height + ThreadGroupSizeY - 1) / ThreadGroupSizeY}
{
}

void ConvergenceMeasurePass::InitRenderResources()
{
    compute_shader_ = rhi_->CreateShader<ConvergenceMeasureShader>();

    pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ConvergenceMeasurePipeline");
    pipeline_state_->SetShader<RHIShaderStage::Compute>(compute_shader_);
    pipeline_state_->Compile();

    uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ConvergenceMeasureShader::UniformBufferData),
                                          .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                          .mem_properties = RHIMemoryProperty::None,
                                          .is_dynamic = true},
                                         "ConvergenceMeasureUniformBuffer");

    compute_pass_ = rhi_->CreateComputePass("ConvergenceMeasureComputePass", false);

    const auto tile_count = tile_count_.x() * tile_count_.y();
    const auto buffer_size = static_cast<size_t>(tile_count) * sizeof(TileMetrics);
    const auto frames_in_flight = rhi_->GetMaxFramesInFlight();
    tile_metric_buffers_.reserve(frames_in_flight);
    for (auto i = 0u; i < frames_in_flight; ++i)
    {
        tile_metric_buffers_.push_back(
            rhi_->CreateBuffer({.size = buffer_size,
                                .usages = RHIBuffer::BufferUsage::StorageBuffer,
                                .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                .is_dynamic = false},
                               "ConvergenceTileMetrics" + std::to_string(i)));
    }

    auto make_variance_state_image = [this](uint32_t index) {
        return rhi_->CreateImage(
            RHIImage::Attribute{
                .format = PixelFormat::RG32Float,
                .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                            .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                            .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
                .width = image_size_.x(),
                .height = image_size_.y(),
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
            },
            "ConvergenceVarianceState" + std::to_string(index));
    };

    variance_state_images_[0] = make_variance_state_image(0);
    variance_state_images_[1] = make_variance_state_image(1);
}

void ConvergenceMeasurePass::Reset()
{
    variance_state_index_ = 0;
    measurement_count_ = 0;
    accumulated_sample_count_ = 0;
    variance_state_initialized_ = false;
    std::lock_guard<std::mutex> lock(async_state_->mutex);
    ++async_state_->generation;
    async_state_->history.clear();
    async_state_->latest_metrics = {};
}

void ConvergenceMeasurePass::Measure(RHIImage *sample_stats_image, RHIPipelineStage sample_stats_after_stage,
                                     uint32_t sample_count, uint32_t batch_sample_count)
{
    ASSERT(sample_stats_image);
    ASSERT(batch_sample_count > 0);
    ASSERT(sample_count >= batch_sample_count);

    const auto frame_index = rhi_->GetFrameIndex();
    ASSERT(frame_index < tile_metric_buffers_.size());

    const auto read_index = variance_state_index_;
    const auto write_index = 1u - variance_state_index_;
    const auto previous_measurement_count = measurement_count_;
    const auto current_measurement_count = previous_measurement_count + 1u;
    const auto previous_sample_count = sample_count - batch_sample_count;

    if (sample_count <= accumulated_sample_count_)
    {
        return;
    }

    auto *resources = pipeline_state_->GetShaderResource<ConvergenceMeasureShader>();
    resources->ubo().BindResource(uniform_buffer_);
    resources->currentSampleStats().BindResource(sample_stats_image->GetDefaultView(rhi_));
    resources->previousVarianceState().BindResource(variance_state_images_[read_index]->GetDefaultView(rhi_));
    resources->nextVarianceState().BindResource(variance_state_images_[write_index]->GetDefaultView(rhi_));
    resources->outTileMetrics().BindResource(tile_metric_buffers_[frame_index]);

    ConvergenceMeasureShader::UniformBufferData ubo{
        .resolution = {image_size_.x(), image_size_.y()},
        .previous_sample_count = previous_sample_count,
        .current_batch_sample_count = batch_sample_count,
        .tile_count_x = tile_count_.x(),
    };
    uniform_buffer_->Upload(rhi_, &ubo);

    sample_stats_image->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = sample_stats_after_stage,
                                    .before_stage = RHIPipelineStage::ComputeShader});
    variance_state_images_[read_index]->Transition(
        {.target_layout = RHIImageLayout::Read,
         .after_stage = variance_state_initialized_ ? RHIPipelineStage::ComputeShader : RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::ComputeShader});
    variance_state_images_[write_index]->Transition(
        {.target_layout = RHIImageLayout::StorageWrite,
         .after_stage = variance_state_initialized_ ? RHIPipelineStage::ComputeShader : RHIPipelineStage::Top,
         .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(pipeline_state_, {image_size_.x(), image_size_.y(), 1u},
                          {ThreadGroupSizeX, ThreadGroupSizeY, 1u});
    rhi_->EndComputePass(compute_pass_);

    variance_state_images_[write_index]->Transition({.target_layout = RHIImageLayout::Read,
                                                     .after_stage = RHIPipelineStage::ComputeShader,
                                                     .before_stage = RHIPipelineStage::ComputeShader});
    variance_state_index_ = write_index;
    measurement_count_ = current_measurement_count;
    accumulated_sample_count_ = sample_count;
    variance_state_initialized_ = true;

    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(async_state_->mutex);
        generation = async_state_->generation;
    }

    auto state = async_state_;
    auto metrics_buffer = tile_metric_buffers_[frame_index];
    const auto tile_count = tile_count_.x() * tile_count_.y();
    rhi_->EnqueueEndOfRenderTasks([state, metrics_buffer, tile_count, sample_count, current_measurement_count,
                                   generation]() {
        const auto *raw_metrics = reinterpret_cast<const ConvergenceMeasurePass::TileMetrics *>(metrics_buffer->Lock());

        double total_variance = 0.0;
        uint64_t active_pixels = 0;
        float max_variance = 0.f;

        for (auto i = 0u; i < tile_count; ++i)
        {
            const auto &tile = raw_metrics[i];
            total_variance += tile.sum_variance;
            active_pixels += tile.active_pixels;
            max_variance = std::max(max_variance, tile.max_variance);
        }

        metrics_buffer->UnLock();

        PerformanceMetrics metrics;
        metrics.sample_count = sample_count;
        metrics.frame_count = current_measurement_count;
        metrics.compared_pixels = static_cast<uint32_t>(active_pixels);

        if (active_pixels > 0)
        {
            metrics.mean_luma_variance = static_cast<float>(total_variance / static_cast<double>(active_pixels));
            metrics.max_luma_variance = max_variance;
        }

        const bool metrics_finite =
            std::isfinite(metrics.mean_luma_variance) && std::isfinite(metrics.max_luma_variance);
        if (!metrics_finite)
        {
            metrics = {};
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->generation == generation)
        {
            if (active_pixels > 0 && metrics_finite)
            {
                const auto half_sample_count = sample_count / 2u;
                const HistoryEntry *reference = nullptr;
                for (auto it = state->history.rbegin(); it != state->history.rend(); ++it)
                {
                    if (it->sample_count <= half_sample_count)
                    {
                        reference = &(*it);
                        break;
                    }
                }

                if (reference != nullptr)
                {
                    metrics.mean_variance_improvement =
                        std::max(reference->mean_luma_variance - metrics.mean_luma_variance, 0.f);
                    metrics.max_variance_improvement =
                        std::max(reference->max_luma_variance - metrics.max_luma_variance, 0.f);
                    metrics.mean_relative_variance_improvement =
                        metrics.mean_variance_improvement / std::max(reference->mean_luma_variance, 1e-6f);
                    metrics.max_relative_variance_improvement =
                        metrics.max_variance_improvement / std::max(reference->max_luma_variance, 1e-6f);
                    metrics.valid = true;
                }

                if (!state->history.empty() && state->history.back().sample_count == sample_count)
                {
                    state->history.back() = {sample_count, metrics.mean_luma_variance, metrics.max_luma_variance};
                }
                else
                {
                    state->history.push_back({sample_count, metrics.mean_luma_variance, metrics.max_luma_variance});
                }
            }

            state->latest_metrics = metrics;
        }
    });
}

PerformanceMetrics ConvergenceMeasurePass::GetLatestMetrics() const
{
    std::lock_guard<std::mutex> lock(async_state_->mutex);
    return async_state_->latest_metrics;
}

void ConvergenceMeasurePass::UpdateLatestMetrics(const PerformanceMetrics &metrics) const
{
    std::lock_guard<std::mutex> lock(async_state_->mutex);
    async_state_->latest_metrics = metrics;
}

void ConvergenceMeasurePass::InvalidateLatestMetrics(uint32_t sample_count, uint32_t frame_count) const
{
    UpdateLatestMetrics({.valid = false, .sample_count = sample_count, .frame_count = frame_count});
}
} // namespace sparkle
