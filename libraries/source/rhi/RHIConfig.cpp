#include "rhi/RHIConfig.h"

#include "application/ConfigCollectionHelper.h"

namespace sparkle
{
static ConfigValue<bool> config_vsync("vsync", "enable vsync (default=0)", "rhi", false, true);
static ConfigValue<std::string> config_rhi("rhi", "what rhi to use (vulkan, metal)", "rhi",
                                           Enum2Str<RHIConfig::ApiPlatform::Vulkan>());
static ConfigValue<bool> config_validation("validation", "RHI validation (default=1)", "rhi", false);
static ConfigValue<bool> config_validate_sync("validate_sync", "Vulkan synchronization validation, needs validation",
                                              "rhi", false);
static ConfigValue<bool> config_pre_transform("vulkan.android.pretransform", "enable vulkan pretransform for android",
                                              "rhi", true);
static ConfigValue<std::string> config_upload_replay(
    "vulkan.upload_replay",
    "copy each upload into a new read-only texture again after the frame (auto, on, off); auto = on on the Apple "
    "Paravirtual device",
    "rhi", Enum2Str<RHIConfig::UploadReplay::Auto>());
static ConfigValue<bool> config_measure_gpu_time("measure_gpu_time", "measure gpu time", "rhi", true);
static ConfigValue<bool> config_sampler_anisotropy("sampler_anisotropy",
                                                   "debug: let samplers filter anisotropically; off creates every "
                                                   "sampler without anisotropy",
                                                   "rhi", true);

void RHIConfig::Init()
{
    ConfigCollectionHelper::RegisterConfig(this, config_rhi, api_platform);
    ConfigCollectionHelper::RegisterConfig(this, config_vsync, use_vsync);
    ConfigCollectionHelper::RegisterConfig(this, config_validation, enable_validation);
    ConfigCollectionHelper::RegisterConfig(this, config_validate_sync, enable_sync_validation);
    ConfigCollectionHelper::RegisterConfig(this, config_pre_transform, enable_pre_transform);
    ConfigCollectionHelper::RegisterConfig(this, config_upload_replay, upload_replay);
    ConfigCollectionHelper::RegisterConfig(this, config_measure_gpu_time, measure_gpu_time);
    ConfigCollectionHelper::RegisterConfig(this, config_sampler_anisotropy, sampler_anisotropy);

    Validate();
}

void RHIConfig::Validate()
{
#if FRAMEWORK_APPLE
    if (api_platform != RHIConfig::ApiPlatform::Metal)
    {
        Log(Warn, "Only Metal is support on Apple platform. Use Metal instead.");
        api_platform = RHIConfig::ApiPlatform::Metal;
        config_rhi.Set(Enum2Str<RHIConfig::ApiPlatform>(api_platform));
    }
#endif

    if (api_platform == RHIConfig::ApiPlatform::None)
    {
        DumpAndAbort();
    }

    bool support_pre_transform = FRAMEWORK_ANDROID && api_platform == RHIConfig::ApiPlatform::Vulkan;
    if (!support_pre_transform && enable_pre_transform)
    {
        Log(Warn, "Pretransform is only supported on android vulkan. Disabling.");
        config_pre_transform.Set(false);
        enable_pre_transform = false;
    }
}
} // namespace sparkle
