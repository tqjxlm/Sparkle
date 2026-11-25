#include "renderer/upscaler/UpscalerFactory.h"

#include "core/Exception.h"
#include "renderer/denoiser/DenoiserConfig.h"
#include "renderer/upscaler/FsrUpscaler.h"

namespace sparkle
{
std::unique_ptr<Upscaler> CreateUpscaler(UpscalerProvider provider, const UpscalerDesc &desc, RHIContext *rhi)
{
    switch (provider)
    {
    case UpscalerProvider::Fsr: {
        auto upscaler = std::make_unique<FsrUpscaler>(rhi, desc);
        if (!upscaler->IsReady())
        {
            return nullptr;
        }
        return upscaler;
    }
    // auto is a selection policy and MetalFX an integrated provider, neither a split upscaler
    case UpscalerProvider::Off:
    case UpscalerProvider::Auto:
    case UpscalerProvider::MetalFx:
        return nullptr;
    default:
        UnImplemented(provider);
        return nullptr;
    }
}
} // namespace sparkle
