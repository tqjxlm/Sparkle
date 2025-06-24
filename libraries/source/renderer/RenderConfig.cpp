#include "renderer/RenderConfig.h"

#include "core/Logger.h"
#include "rhi/RHI.h"
#if FRAMEWORK_ANDROID || FRAMEWORK_IOS
#include "application/NativeView.h"
#endif
#include "application/ConfigCollectionHelper.h"

namespace sparkle
{
static ConfigValue<std::string> config_pipeline("pipeline", "render pipeline", "renderer",
                                                Enum2Str<RenderConfig::Pipeline::forward>(), true);
static ConfigValue<std::string> config_output_image("output_image", "output image", "renderer",
                                                    Enum2Str<RenderConfig::OutputImage::SceneColor>(), true);
static ConfigValue<std::string> config_debug_mode("debug_mode", "debug mode", "renderer",
                                                  Enum2Str<RenderConfig::DebugMode::Color>(), true);
static ConfigValue<uint32_t> config_spp("spp", "num rays per sample per frame", "renderer", 1);
static ConfigValue<bool> config_dynamic_spp("dynamic_spp", "use dynamic spp depending on framerate", "renderer", false,
                                            true);
static ConfigValue<uint32_t> config_max_spp("max_spp", "maximum num rays per sample accumulated", "renderer", 2048);
static ConfigValue<uint32_t> config_width("width", "image width", "renderer", 1280);
static ConfigValue<uint32_t> config_height("height", "image height", "renderer", 720);
static ConfigValue<uint32_t> config_bounce("bounce", "num intersections allowed per ray", "renderer", 8);
static ConfigValue<bool> config_ssao("ssao", "enable ssao", "renderer", false, true);
static ConfigValue<bool> config_diffuse_ibl("diffuse_ibl", "enable diffuse ibl", "renderer", true, true);
static ConfigValue<bool> config_specular_ibl("specular_ibl", "enable specular ibl", "renderer", true, true);
static ConfigValue<bool> config_prepass("prepass", "enable prepass", "renderer", false, true);
static ConfigValue<uint32_t> config_shadow_map_resolution("shadow_map_resolution", "shadow map resolution", "renderer",
                                                          1024);
static ConfigValue<bool> config_spatial_denoise("spatial_denoise", "use spatial denoise in ray tracing procedures",
                                                "renderer", true, true);
static ConfigValue<float> config_target_framerate("target_framerate", "target frame rate", "renderer", 60.f);
static ConfigValue<float> config_gpu_budget_ratio("gpu_time_budget_ratio", "GPU time budget ratio for ray tracing",
                                                  "renderer", 0.8f);

void RenderConfig::Init()
{
    ConfigCollectionHelper::RegisterConfig(this, config_pipeline, pipeline);
    ConfigCollectionHelper::RegisterConfig(this, config_output_image, output_image);
    ConfigCollectionHelper::RegisterConfig(this, config_debug_mode, debug_mode);
    ConfigCollectionHelper::RegisterConfig(this, config_spp, sample_per_pixel);
    ConfigCollectionHelper::RegisterConfig(this, config_bounce, max_bounce);
    ConfigCollectionHelper::RegisterConfig(this, config_max_spp, max_sample_per_pixel);
    ConfigCollectionHelper::RegisterConfig(this, config_ssao, use_ssao);
    ConfigCollectionHelper::RegisterConfig(this, config_diffuse_ibl, use_diffuse_ibl);
    ConfigCollectionHelper::RegisterConfig(this, config_specular_ibl, use_specular_ibl);
    ConfigCollectionHelper::RegisterConfig(this, config_prepass, use_prepass);
    ConfigCollectionHelper::RegisterConfig(this, config_width, image_width);
    ConfigCollectionHelper::RegisterConfig(this, config_height, image_height);
    ConfigCollectionHelper::RegisterConfig(this, config_spatial_denoise, spatial_denoise);
    ConfigCollectionHelper::RegisterConfig(this, config_shadow_map_resolution, shadow_map_resolution);
    ConfigCollectionHelper::RegisterConfig(this, config_dynamic_spp, use_dynamic_spp);
    ConfigCollectionHelper::RegisterConfig(this, config_target_framerate, target_framerate);
    ConfigCollectionHelper::RegisterConfig(this, config_gpu_budget_ratio, gpu_time_budget_ratio);

    Validate();
}

static RenderConfig::Pipeline GetFallbackRenderMode(RenderConfig::Pipeline mode)
{
    switch (mode)
    {
    case RenderConfig::Pipeline::gpu:
    case RenderConfig::Pipeline::deferred:
        return RenderConfig::Pipeline::forward;
    default:
        return mode;
    }
}

void RenderConfig::Validate()
{
    if (rhi_ != nullptr && IsRayTracingMode())
    {
        if (!rhi_->SupportsHardwareRayTracing())
        {
            Log(Warn, "hardware ray tracing not supported. fallback to forward rendering");
            pipeline = GetFallbackRenderMode(pipeline);
            config_pipeline.Set(Enum2Str<Pipeline>(pipeline));
        }
    }

    if (pipeline == Pipeline::cpu || pipeline == Pipeline::gpu)
    {
        if (use_prepass)
        {
            Log(Warn, "prepass will not work in mode {}. set to 0", static_cast<int>(pipeline));
            config_prepass.Set(false);
            use_prepass = false;
        }
        if (use_ssao)
        {
            Log(Warn, "ssao will not work in mode {}. set to 0", static_cast<int>(pipeline));
            config_ssao.Set(false);
            use_ssao = false;
        }
    }

    if (use_ssao)
    {
        if (!use_prepass)
        {
            Log(Warn, "ssao requires prepass. set to 1");
            config_prepass.Set(false);
            use_prepass = true;
        }
    }

#if FRAMEWORK_ANDROID || FRAMEWORK_IOS
    if (view_)
    {
        // for mobile platforms, it is always full-screen. we calculate width given height
        // TODO(tqjxlm): full screen support for desktop platforms
        int back_buffer_width;
        int back_buffer_height;
        view_->GetFrameBufferSize(back_buffer_width, back_buffer_height);
        image_width = image_height * (static_cast<float>(back_buffer_width) / back_buffer_height);
        config_width.Set(image_width);

        Log(Info, "View size [{}, {}]", image_width, image_height);
    }
#endif
}
} // namespace sparkle
