#pragma once

#include <cstdint>
#include <memory>

namespace sparkle
{
class Upscaler;
class RHIContext;
enum class UpscalerProvider : uint8_t;
struct UpscalerDesc;

// returns null for a provider this build cannot serve as a split upscaler
std::unique_ptr<Upscaler> CreateUpscaler(UpscalerProvider provider, const UpscalerDesc &desc, RHIContext *rhi);
} // namespace sparkle
