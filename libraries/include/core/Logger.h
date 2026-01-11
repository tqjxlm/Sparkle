#pragma once

#if _MSC_VER
// to make msvc happy...
#define FMT_USE_NONTYPE_TEMPLATE_ARGS 0
#endif

#define LOG_DEBUG_IN_RELEASE 0

#if DEBUG_BUILD || LOG_DEBUG_IN_RELEASE
#define SPDLOG_ACTIVE_LEVEL 1
#else
#define SPDLOG_ACTIVE_LEVEL 2
#endif

#include <spdlog/spdlog.h>

#include <memory>

namespace sparkle
{
class UiManager;

class Logger
{
public:
    Logger();

    ~Logger();

    void DrawUi(UiManager *ui_manager) const;

    static void Flush();

    // thread-safe. it renders in main thread and respects AppConfig::show_screen_log.
    // you should call it every frame to make the message stay on screen.
    static void LogToScreen(const std::string &tag, const std::string &message);

private:
    [[nodiscard]] std::vector<std::string> GetScreenLogs() const
    {
        std::vector<std::string> logs;
        logs.reserve(screen_log_tags_.size());

        for (const auto &tag : screen_log_tags_)
        {
            logs.push_back(screen_logs_.at(tag));
        }

        return logs;
    }

    // we use a shared_ptr here because spdlog will share the ownership
    std::shared_ptr<spdlog::logger> logger_;

    std::unordered_map<std::string, std::string> screen_logs_;
    std::vector<std::string> screen_log_tags_;

    static Logger *instance_;
};

enum class Verbosity : uint8_t
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};
} // namespace sparkle

#define Log(Level, fmt, ...)                                                                                           \
    {                                                                                                                  \
        if constexpr (sparkle::Verbosity::Level == sparkle::Verbosity::Debug)                                          \
        {                                                                                                              \
            SPDLOG_DEBUG(fmt __VA_OPT__(, ) __VA_ARGS__);                                                              \
        }                                                                                                              \
        else if constexpr (sparkle::Verbosity::Level == sparkle::Verbosity::Info)                                      \
        {                                                                                                              \
            SPDLOG_INFO(fmt __VA_OPT__(, ) __VA_ARGS__);                                                               \
        }                                                                                                              \
        else if constexpr (sparkle::Verbosity::Level == sparkle::Verbosity::Warn)                                      \
        {                                                                                                              \
            SPDLOG_WARN(fmt __VA_OPT__(, ) __VA_ARGS__);                                                               \
        }                                                                                                              \
        else if constexpr (sparkle::Verbosity::Level == sparkle::Verbosity::Error)                                     \
        {                                                                                                              \
            SPDLOG_ERROR(fmt __VA_OPT__(, ) __VA_ARGS__);                                                              \
        }                                                                                                              \
    }
