#include "rhi/RHIConfig.h"

#include "application/ConfigCollectionHelper.h"

namespace sparkle
{
static ConfigValue<bool> config_vsync("vsync", "enable vsync (default=0)", "rhi", false, true);
static ConfigValue<uint32_t> config_msaa("msaa", "MSAA sample count (default=1)", "rhi", 1);
static ConfigValue<std::string> config_rhi("rhi", "what rhi to use (vulkan, metal)", "rhi",
                                           Enum2Str<RHIConfig::ApiPlatform::Vulkan>());
static ConfigValue<bool> config_validation("validation", "RHI validation (default=1)", "rhi", false);
static ConfigValue<bool> config_pre_transform("vulkan.android.pretransform", "enable vulkan pretransform for android",
                                              "rhi", true);
static ConfigValue<bool> config_measure_gpu_time("measure_gpu_time", "measure gpu time", "rhi", true);

void RHIConfig::Init()
{
    ConfigCollectionHelper::RegisterConfig(this, config_rhi, api_platform);
    ConfigCollectionHelper::RegisterConfig(this, config_vsync, use_vsync);
    ConfigCollectionHelper::RegisterConfig(this, config_msaa, msaa_samples);
    ConfigCollectionHelper::RegisterConfig(this, config_validation, enable_validation);
    ConfigCollectionHelper::RegisterConfig(this, config_pre_transform, enable_pre_transform);
    ConfigCollectionHelper::RegisterConfig(this, config_measure_gpu_time, measure_gpu_time);

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

    auto nearest_valid_msaa = msaa_samples;
    for (int i = 0; i < 7; i++)
    {
        uint32_t test_bit = 1u >> i;
        if (nearest_valid_msaa & test_bit)
        {
            break;
        }

        nearest_valid_msaa &= ~test_bit;
    }

    if (nearest_valid_msaa != msaa_samples)
    {
        Log(Warn, "invalid msaa sample {}. round down to {}", msaa_samples, nearest_valid_msaa);
        config_msaa.Set(msaa_samples);
        msaa_samples = nearest_valid_msaa;
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
