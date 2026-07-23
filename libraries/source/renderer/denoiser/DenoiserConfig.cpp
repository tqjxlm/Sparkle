#include "renderer/denoiser/DenoiserConfig.h"

#include "application/ConfigCollectionHelper.h"
#include "core/ConfigValue.h"

namespace sparkle
{
static ConfigValue<std::string> config_denoiser("denoiser", "gpu path-tracer denoiser: off, auto, nrd, or metalfx",
                                                "denoiser", Enum2Str<DenoiserProvider::Off>(), true);
static ConfigValue<bool> config_nrd_radiance_fp16(
    "nrd_radiance_fp16", "RGBA16F shared noisy-radiance inputs (half the bandwidth; init-time)", "denoiser", true);
static ConfigValue<bool> config_metalfx_sync_init(
    "metalfx_sync_init", "compile the MetalFX denoiser synchronously during renderer initialization", "denoiser",
    false);

DenoiserConfig &DenoiserConfig::Get()
{
    static DenoiserConfig instance;
    [[maybe_unused]] static const bool Initialized = (instance.Init(), true);
    return instance;
}

void DenoiserConfig::Init()
{
    ConfigCollectionHelper::RegisterConfig(this, config_denoiser, provider);
    ConfigCollectionHelper::RegisterConfig(this, config_nrd_radiance_fp16, radiance_fp16);
    ConfigCollectionHelper::RegisterConfig(this, config_metalfx_sync_init, metalfx_sync_init);
}
} // namespace sparkle
