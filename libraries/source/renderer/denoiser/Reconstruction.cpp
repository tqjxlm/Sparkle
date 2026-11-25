#include "renderer/denoiser/Reconstruction.h"

#include <algorithm>

namespace sparkle
{
namespace
{
float Halton(uint32_t index, uint32_t base)
{
    float fraction = 1.f;
    float result = 0.f;
    while (index > 0)
    {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}
} // namespace

ReconstructionPlan PlanReconstruction(DenoiserProvider denoiser, UpscalerProvider upscaler)
{
    const bool names_integrated = denoiser == DenoiserProvider::MetalFx || upscaler == UpscalerProvider::MetalFx;
    // auto prefers the integrated provider unless the upscaler asks for a split stage
    const bool auto_integrated = denoiser == DenoiserProvider::Auto && upscaler != UpscalerProvider::Fsr;
    return {
        .integrated = names_integrated || auto_integrated,
        .denoiser = denoiser == DenoiserProvider::Off ? DenoiserProvider::Off : DenoiserProvider::Nrd,
        .upscaler = upscaler == UpscalerProvider::Off ? UpscalerProvider::Off : UpscalerProvider::Fsr,
    };
}

Vector2 ComputeJitter(uint32_t index, const Vector2UInt &input_size, const Vector2UInt &output_size)
{
    constexpr float BasePhaseCount = 8.f;
    const float scale = static_cast<float>(output_size.x()) / static_cast<float>(input_size.x());
    const auto phase_count = std::max(1u, static_cast<uint32_t>(BasePhaseCount * scale * scale));
    const uint32_t phase = index % phase_count + 1;
    return {Halton(phase, 2) - 0.5f, Halton(phase, 3) - 0.5f};
}
} // namespace sparkle
