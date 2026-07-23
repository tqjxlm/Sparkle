#include "renderer/denoiser/DenoiserFactory.h"

#include "core/Logger.h"
#include "renderer/denoiser/DenoiserConfig.h"
#include "renderer/nrd/NrdDenoiser.h"
#include "rhi/RHI.h"

namespace sparkle
{
std::unique_ptr<RHIDenoiser> CreateDenoiser(DenoiserProvider provider, const RHIDenoiserDesc &desc, RHIContext *rhi)
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
        return rhi->CreatePlatformDenoiser(RHIPlatformDenoiser::MetalFx, desc);
    case DenoiserProvider::Auto:
        Log(Error, "auto is a denoiser selection policy, not a concrete provider");
        return nullptr;
    default:
        UnImplemented(provider);
        return nullptr;
    }
}
} // namespace sparkle
