#pragma once

#include <cstdint>

namespace sparkle
{
// Convergence handoff: a temporal denoiser's capped history re-blends fresh 1-spp noise every
// frame, so a static view would shimmer forever. Every provider therefore cross-fades its
// output into the progressive accumulator as the accumulator converges. The schedule is the
// same for all of them — the denoiser_sweep gate compares providers against each other — so it
// lives here rather than once per provider.
//
// Start after the accumulator's own grain drops below the denoiser's noise floor; fully
// converged by End. Handing over earlier only exposes the accumulator's grain (5-12% at N<400,
// the "dirty" look) while a provider's own output stabilization already suppresses churn in
// that window. Convergence to the exact accumulated image is preserved, just later.
class DenoiserHandoff
{
public:
    static constexpr float StartSamples = 512.f;
    static constexpr float EndSamples = 2048.f;

    // a max_sample_per_pixel below the window opts out entirely: the motion harnesses (max_spp=1)
    // rely on the frozen frame being the last denoised output, not the raw low-spp accumulator.
    // the opt-out is unconditional — a frame's spp is not bounded by the distance left to max_spp,
    // so an opted-out render can still push the accumulator past StartSamples
    explicit DenoiserHandoff(uint32_t max_sample_per_pixel);

    [[nodiscard]] bool Applies() const
    {
        return applies_;
    }

    [[nodiscard]] float GetEnd() const
    {
        return end_;
    }

    // 0 keeps the denoised output, 1 shows the accumulator. final_frame pins the weight to 1 so
    // the frozen frame IS the accumulator, bit-exact
    [[nodiscard]] float ComputeWeight(float accumulated_samples, bool final_frame) const;

private:
    bool applies_;
    float start_;
    float end_;
};
} // namespace sparkle
