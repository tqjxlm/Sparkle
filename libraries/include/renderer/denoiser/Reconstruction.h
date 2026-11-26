#pragma once

#include "core/math/Types.h"
#include "renderer/denoiser/DenoiserConfig.h"

namespace sparkle
{
// The providers the gpu path tracer tries to reconstruct its frame with. A provider that denoises and upscales in one
// step (MetalFX) serves both stages, so naming it on either side selects it for both. Otherwise the stages are split:
// a denoiser at scene resolution, then an upscaler to output resolution.
struct ReconstructionPlan
{
    // the integrated provider is tried first; the split stages are its fallback
    bool integrated = false;
    DenoiserProvider denoiser = DenoiserProvider::Off;
    UpscalerProvider upscaler = UpscalerProvider::Off;

    bool operator==(const ReconstructionPlan &) const = default;
};

[[nodiscard]] ReconstructionPlan PlanReconstruction(DenoiserProvider denoiser, UpscalerProvider upscaler);

// the sub-pixel camera jitter of frame `index`, in pixels within [-0.5, 0.5]: a Halton(2, 3) sequence whose period
// grows with the square of the upscale factor, so the jittered samples cover every output pixel
[[nodiscard]] Vector2 ComputeJitter(uint32_t index, const Vector2UInt &input_size, const Vector2UInt &output_size);
} // namespace sparkle
