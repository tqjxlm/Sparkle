#include "core/Logger.h"

#include "core/Exception.h"
#include "core/FileManager.h"
#include "core/TaskManager.h"

#if FRAMEWORK_ANDROID
#include <spdlog/sinks/android_sink.h>
#endif
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>

namespace sparkle
{
Logger *Logger::instance_ = nullptr;

Logger::Logger()
{
    ASSERT(!instance_);

    instance_ = this;

    logger_ = std::make_shared<spdlog::logger>("default");

    FileManager::GetNativeFileManager()->TryCreateDirectory("logs", true);

    {
        namespace ch = std::chrono;
        auto time_stamp = std::format("{:%Y_%m_%d_%H_%M_%S}", ch::floor<ch::seconds>(ch::system_clock::now()));

        auto log_file_path_relative = std::format("logs/output_{}.log", time_stamp);
        auto log_file_path = FileManager::GetNativeFileManager()->GetAbosluteFilePath(log_file_path_relative, true);

        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file_path, true);
        logger_->sinks().push_back(file_sink);
    }

    // TODO(tqjxlm): copy the file instead of writing to two files at runtime
    {
        auto log_file_path_relative = std::string("logs/output.log");
        auto log_file_path = FileManager::GetNativeFileManager()->GetAbosluteFilePath(log_file_path_relative, true);

        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file_path, true);
        logger_->sinks().push_back(file_sink);
    }

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    logger_->sinks().push_back(console_sink);

#if FRAMEWORK_ANDROID
    auto android_sink = std::make_shared<spdlog::sinks::android_sink_mt>("sparkle");
    logger_->sinks().push_back(android_sink);
#endif

#if LOG_DEBUG_IN_RELEASE || !defined(NDEBUG)
    logger_->set_level(spdlog::level::debug);
#else
    logger_->set_level(spdlog::level::info);
#endif

    spdlog::set_default_logger(logger_);
}

Logger::~Logger()
{
    logger_->flush();

    instance_ = nullptr;
}

void Logger::Flush()
{
    spdlog::default_logger()->flush();
}

void Logger::LogToScreen(const std::string &tag, const std::string &message)
{
    ASSERT(instance_);

    TaskManager::RunInMainThread([tag, message]() {
        auto entry = instance_->screen_logs_.find(tag);
        if (entry == instance_->screen_logs_.end())
        {
            ASSERT(!message.empty());
            instance_->screen_log_tags_.push_back(tag);
            instance_->screen_logs_.emplace(tag, message);
        }
        else
        {
            if (message.empty())
            {
                instance_->screen_logs_.erase(entry);
                instance_->screen_log_tags_.erase(
                    std::find(instance_->screen_log_tags_.begin(), instance_->screen_log_tags_.end(), tag));
            }
            else
            {
                entry->second = message;
            }
        }
    });
}
} // namespace sparkle
