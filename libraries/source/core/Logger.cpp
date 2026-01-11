#include "core/Logger.h"

#include "application/UiManager.h"
#include "core/Exception.h"
#include "core/FileManager.h"
#include "core/task/TaskManager.h"

#if FRAMEWORK_ANDROID
#include <spdlog/sinks/android_sink.h>
#endif
#include <imgui.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
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
                instance_->screen_log_tags_.erase(std::ranges::find(instance_->screen_log_tags_, tag));
            }
            else
            {
                entry->second = message;
            }
        }
    });
}

void Logger::DrawUi(UiManager *ui_manager) const
{
    auto messages = GetScreenLogs();
    if (!messages.empty())
    {
        ui_manager->RequestWindowDraw({[messages]() {
            float font_size = ImGui::GetFontSize();

            auto max_width = font_size * 30;
            auto max_height = font_size * 20;

            const ImGuiViewport *main_viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + main_viewport->WorkSize.x - max_width - 20,
                                           main_viewport->WorkPos.y + 20),
                                    ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(max_width, max_height), ImGuiCond_Always);

            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;

            ImGui::Begin("Screen Log", nullptr, window_flags);

            for (const auto &log : messages)
            {
                // right align
                float window_width = ImGui::GetWindowWidth();
                float text_width = ImGui::CalcTextSize(log.c_str()).x;
                ImGui::SetCursorPosX(window_width - text_width);

                ImGui::TextUnformatted(log.c_str());
            }

            ImGui::End();
        }});
    }
}
} // namespace sparkle
