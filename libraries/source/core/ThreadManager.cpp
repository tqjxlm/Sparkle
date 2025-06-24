#include "core/ThreadManager.h"

#include "core/Logger.h"
#include "core/ThreadUtils.h"

namespace sparkle
{
std::thread::id ThreadManager::main_thread_id_;
std::thread::id ThreadManager::render_thread_id_;
static thread_local std::string thread_name = "UnknownThread";

void ThreadManager::RegisterRenderThread()
{
    ASSERT(render_thread_id_ == std::thread::id());
    render_thread_id_ = std::this_thread::get_id();

    std::stringstream ss;
    ss << std::this_thread::get_id();
    Log(Info, "render thread id: {}", ss.str());

    thread_name = "RenderThread";
    SetCurrentThreadName(thread_name);
}

void ThreadManager::RegisterMainThread()
{
    ASSERT(main_thread_id_ == std::thread::id());
    main_thread_id_ = std::this_thread::get_id();

    std::stringstream ss;
    ss << std::this_thread::get_id();
    Log(Info, "main thread id: {}", ss.str());

    thread_name = "MainThread";
    SetCurrentThreadName(thread_name);
}

void ThreadManager::PrintCurrentThreadInfo()
{
    std::stringstream ss;
    ss << std::this_thread::get_id();
    Log(Info, "current thread: id {}. name {}.", ss.str(), thread_name);
}

void ThreadManager::UnregisterRenderThread()
{
    render_thread_id_ = std::thread::id();
}

void ThreadManager::RegisterTaskThread(size_t thread_index)
{
    thread_name = "TaskThread" + std::to_string(thread_index);
    SetCurrentThreadName(thread_name);
}
} // namespace sparkle
