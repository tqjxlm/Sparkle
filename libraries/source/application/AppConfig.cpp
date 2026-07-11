#include "application/AppConfig.h"

#include "application/ConfigCollectionHelper.h"

namespace sparkle
{
static ConfigValue<uint32_t> config_max_thread("thread", "maximum threads to use", "app", 64);
// Empty (the default) => the built-in TestScene, which is also the scene the CI ground-truth images
// are rendered from. A non-empty value is a model/scene file path (under resources/), not a scene name.
static ConfigValue<std::string> config_scene("scene", "scene file path to render; empty = built-in TestScene", "app",
                                             "");
static ConfigValue<bool> config_screen_log("screen_log", "show screen log", "app", true, true);
static ConfigValue<bool> config_rebuild_cache("rebuild_cache", "rebuild all cache", "app", false);
static ConfigValue<bool> config_default_skybox("default_sky", "use a default sky box", "app", false, true);
static ConfigValue<bool> config_render_thread("render_thread", "enable render thread", "app", true);
static ConfigValue<bool> config_load_last_session("load_last_session",
                                                  "load last session on startup, including all configs and camera "
                                                  "state. this will override current command line arguments.",
                                                  "app", false);
static ConfigValue<bool> config_headless("headless", "run without creating a window (desktop only)", "app", false);

#if ENABLE_TEST_CASES
static ConfigValue<std::string> config_test_case("test_case", "name of test case to run on scene load", "app", "");
static ConfigValue<uint32_t> config_test_timeout("test_timeout",
                                                 "max frames before a test case is considered timed out (0 = no limit)",
                                                 "app", 0);
#endif

void AppConfig::Init()
{
#if PLATFORM_MACOS
    platform = NativePlatform::MacOS;
#elif PLATFORM_IOS
    platform = NativePlatform::iOS;
#elif PLATFORM_ANDROID
    platform = NativePlatform::Android;
#elif PLATFORM_WINDOWS
    platform = NativePlatform::Windows;
#elif PLATFORM_LINUX
    platform = NativePlatform::Linux;
#else
    static_assert(false, "No valid platform is provided.");
#endif

    ConfigCollectionHelper::RegisterConfig(this, config_max_thread, max_threads);
    ConfigCollectionHelper::RegisterConfig(this, config_scene, scene);
    ConfigCollectionHelper::RegisterConfig(this, config_screen_log, show_screen_log);
    ConfigCollectionHelper::RegisterConfig(this, config_rebuild_cache, rebuild_cache);
    ConfigCollectionHelper::RegisterConfig(this, config_default_skybox, default_skybox);
    ConfigCollectionHelper::RegisterConfig(this, config_render_thread, render_thread);
    ConfigCollectionHelper::RegisterConfig(this, config_load_last_session, load_last_session);
    ConfigCollectionHelper::RegisterConfig(this, config_headless, headless);

#if ENABLE_TEST_CASES
    ConfigCollectionHelper::RegisterConfig(this, config_test_case, test_case);
    ConfigCollectionHelper::RegisterConfig(this, config_test_timeout, test_timeout);
#endif
}
} // namespace sparkle
