#include "renderer/upscaler/UpscalerFactory.h"

#include "core/Exception.h"
#include "core/Logger.h"
#include "renderer/denoiser/DenoiserConfig.h"
#if ENABLE_FSR
#include "renderer/upscaler/fsr/FsrUpscaler.h"
#endif

namespace sparkle
{
std::unique_ptr<Upscaler> CreateUpscaler(UpscalerProvider provider, [[maybe_unused]] const UpscalerDesc &desc,
                                         [[maybe_unused]] RHIContext *rhi)
{
    switch (provider)
    {
    case UpscalerProvider::Fsr: {
#if ENABLE_FSR
        auto upscaler = std::make_unique<FsrUpscaler>(rhi, desc);
        if (!upscaler->IsReady())
        {
            return nullptr;
        }
        return upscaler;
#else
        Log(Warn, "FSR is not part of this build (ENABLE_FSR=OFF)");
        return nullptr;
#endif
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
