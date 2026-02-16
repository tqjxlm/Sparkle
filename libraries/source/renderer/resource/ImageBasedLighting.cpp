#include "renderer/resource/ImageBasedLighting.h"

#include "core/Profiler.h"
#include "renderer/RenderConfig.h"
#include "renderer/pass/IBLBrdfPass.h"
#include "renderer/pass/IBLDiffusePass.h"
#include "renderer/pass/IBLSpecularPass.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <array>

namespace sparkle
{
ImageBasedLighting::ImageBasedLighting(const RHIResourceRef<RHIImage> &env_map) : env_map_(env_map)
{
}

ImageBasedLighting::~ImageBasedLighting() = default;

void ImageBasedLighting::InitRenderResources(RHIContext *ctx, const RenderConfig &config)
{
    ASSERT(env_map_);

    PROFILE_SCOPE_LOG("ImageBasedLighting::InitRenderResources");

    rhi_ = ctx;

    ibl_brdf_pass_ = PipelinePass::Create<IBLBrdfPass>(config, ctx);
    ibl_diffuse_pass_ = PipelinePass::Create<IBLDiffusePass>(config, ctx, env_map_);
    ibl_specular_pass_ = PipelinePass::Create<IBLSpecularPass>(config, ctx, env_map_);
}

void ImageBasedLighting::CookOnTheFly(const RenderConfig &config)
{
    const unsigned step_budget = GetAdaptiveCookStepBudget(config);
    const std::array<IBLPass *, 3> ibl_passes{ibl_brdf_pass_.get(), ibl_diffuse_pass_.get(), ibl_specular_pass_.get()};

    std::array<size_t, 3> active_pass_indices{};
    size_t active_pass_count = 0;
    for (size_t i = 0; i < ibl_passes.size(); ++i)
    {
        const size_t pass_index = (next_cook_pass_index_ + i) % ibl_passes.size();
        auto *pass = ibl_passes[pass_index];

        if (!pass || pass->IsReady())
        {
            continue;
        }

        active_pass_indices[active_pass_count++] = pass_index;
    }

    if (active_pass_count == 0)
    {
        return;
    }

    const unsigned base_samples = step_budget / static_cast<unsigned>(active_pass_count);
    const unsigned extra_samples = step_budget % static_cast<unsigned>(active_pass_count);

    for (size_t i = 0; i < active_pass_count; ++i)
    {
        const unsigned samples_per_dispatch = base_samples + (i < extra_samples ? 1u : 0u);
        if (samples_per_dispatch == 0)
        {
            continue;
        }

        auto *pass = ibl_passes[active_pass_indices[i]];
        pass->CookOnTheFly(config, samples_per_dispatch);
        if (pass->IsReady())
        {
            render_resource_change_event_.Trigger();
        }
    }

    next_cook_pass_index_ = static_cast<uint8_t>((next_cook_pass_index_ + 1u) % ibl_passes.size());
}

unsigned ImageBasedLighting::GetAdaptiveCookStepBudget(const RenderConfig &config)
{
    constexpr unsigned MinCookStepsPerFrame = 1;
    constexpr unsigned MaxCookStepsPerFrame = 64;
    constexpr float IncreaseThresholdRatio = 0.7f;

    if (!rhi_)
    {
        return cook_steps_per_frame_;
    }

    const auto frame_index = rhi_->GetFrameIndex();
    const float last_frame_gpu_time_ms = rhi_->GetFrameStats(frame_index).elapsed_time_ms;
    if (last_frame_gpu_time_ms <= 0.f)
    {
        return cook_steps_per_frame_;
    }

    const float target_frame_rate = std::max(30.f, config.target_framerate);
    const float target_frame_time_ms = 1000.f / target_frame_rate;

    if (last_frame_gpu_time_ms > target_frame_time_ms)
    {
        // Reduce load fast when frame time exceeds budget.
        cook_steps_per_frame_ = std::max(MinCookStepsPerFrame, cook_steps_per_frame_ / 2u);
    }
    else if (last_frame_gpu_time_ms < target_frame_time_ms * IncreaseThresholdRatio)
    {
        // Increase slowly to avoid oscillation.
        cook_steps_per_frame_ = std::min(MaxCookStepsPerFrame, cook_steps_per_frame_ + 1u);
    }

    return cook_steps_per_frame_;
}
}; // namespace sparkle
