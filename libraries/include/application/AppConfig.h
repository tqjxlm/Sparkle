#pragma once

#include "application/ConfigCollection.h"

namespace sparkle
{
struct AppConfig : public ConfigCollection
{
    enum class NativePlatform : uint8_t
    {
        Windows,
        MacOS,
        iOS,
        Android
    };

    void Init();

    const char *app_name = "Sparkle";
    NativePlatform platform;
    std::string scene;
    uint32_t max_threads;
    bool show_screen_log;
    bool rebuild_cache;
    bool default_skybox;
    bool render_thread;

protected:
    void Validate() override
    {
    }

private:
    friend class AppFramework;
};
} // namespace sparkle
