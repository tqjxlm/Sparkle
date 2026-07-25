#include "renderer/denoiser/DenoiserFactory.h"

#include "core/Logger.h"
#include "renderer/denoiser/DenoiserConfig.h"
#include "renderer/nrd/NrdDenoiser.h"
#include "rhi/RHI.h"

namespace sparkle
{
std::unique_ptr<Denoiser> CreateDenoiser(DenoiserProvider provider, const DenoiserDesc &desc, RHIContext *rhi)
{
    switch (provider)
    {
    case DenoiserProvider::Off:
        return nullptr;
    case DenoiserProvider::Nrd: {
        auto denoiser = std::make_unique<NrdDenoiser>(rhi, desc);
        if (!denoiser->IsReady())
        {
            return nullptr;
        }
        return denoiser;
    }
    case DenoiserProvider::MetalFx:
#if FRAMEWORK_APPLE
        return CreateMetalFxDenoiser(rhi, desc);
#else
        return nullptr;
#endif
    case DenoiserProvider::Auto:
        Log(Error, "auto is a denoiser selection policy, not a concrete provider");
        return nullptr;
    default:
        UnImplemented(provider);
        return nullptr;
    }
}
} // namespace sparkle
