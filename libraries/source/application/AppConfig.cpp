#include "application/AppConfig.h"

#include "application/ConfigCollectionHelper.h"

namespace sparkle
{
static ConfigValue<uint32_t> config_max_thread("thread", "maximum threads to use", "app", 64);
static ConfigValue<std::string> config_scene("scene", "the scene to render", "app", "");
static ConfigValue<bool> config_screen_log("screen_log", "show screen log", "app", true, true);
static ConfigValue<bool> config_rebuild_cache("rebuild_cache", "rebuild all cache", "app", false);
static ConfigValue<bool> config_default_skybox("default_sky", "use a default sky box", "app", false);
static ConfigValue<bool> config_render_thread("render_thread", "enable render thread", "app", true);

void AppConfig::Init()
{
#if FRAMEWORK_MACOS
    platform = NativePlatform::MacOS;
#elif FRAMEWORK_IOS
    platform = NativePlatform::iOS;
#elif FRAMEWORK_ANDROID
    platform = NativePlatform::Android;
#elif PLATFORM_WINDOWS
    platform = NativePlatform::Windows;
#else
#pragma error
#endif

    ConfigCollectionHelper::RegisterConfig(this, config_max_thread, max_threads);
    ConfigCollectionHelper::RegisterConfig(this, config_scene, scene);
    ConfigCollectionHelper::RegisterConfig(this, config_screen_log, show_screen_log);
    ConfigCollectionHelper::RegisterConfig(this, config_rebuild_cache, rebuild_cache);
    ConfigCollectionHelper::RegisterConfig(this, config_default_skybox, default_skybox);
    ConfigCollectionHelper::RegisterConfig(this, config_render_thread, render_thread);
}
} // namespace sparkle
