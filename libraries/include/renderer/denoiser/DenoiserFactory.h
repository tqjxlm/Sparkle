#pragma once

#include <cstdint>
#include <memory>

namespace sparkle
{
class RHIDenoiser;
class RHIContext;
enum class DenoiserProvider : uint8_t;
struct RHIDenoiserDesc;

std::unique_ptr<RHIDenoiser> CreateDenoiser(DenoiserProvider provider, const RHIDenoiserDesc &desc, RHIContext *rhi);
} // namespace sparkle
