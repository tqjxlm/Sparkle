#pragma once

#include <cstdint>
#include <memory>

namespace sparkle
{
class Denoiser;
class RHIContext;
enum class DenoiserProvider : uint8_t;
struct DenoiserDesc;

std::unique_ptr<Denoiser> CreateDenoiser(DenoiserProvider provider, const DenoiserDesc &desc, RHIContext *rhi);

// Defined in rhi/metal/MetalFxDenoiser.mm: MetalFX is reachable only through Objective-C++, so that
// provider's implementation lives with the Metal backend while implementing the same renderer-level
// Denoiser interface. Returns null when the scaler is unsupported.
std::unique_ptr<Denoiser> CreateMetalFxDenoiser(RHIContext *rhi, const DenoiserDesc &desc);
} // namespace sparkle
