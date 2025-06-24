#pragma once

#include "core/Exception.h"

#include <thread>

namespace sparkle
{
enum class ThreadName : uint8_t
{
    Main,
    Render,
    Worker,
};

class ThreadManager
{
public:
    static bool IsInMainThread()
    {
        ASSERT(main_thread_id_ != std::thread::id());
        return std::this_thread::get_id() == main_thread_id_;
    }

    static bool IsInRenderThread()
    {
        return !IsRenderThreadRunning() || std::this_thread::get_id() == render_thread_id_;
    }

    static bool IsRenderThreadRunning()
    {
        return render_thread_id_ != std::thread::id();
    }

    static void RegisterMainThread();

    static void RegisterRenderThread();

    static void RegisterTaskThread(size_t thread_index);

    static void UnregisterRenderThread();

    static void PrintCurrentThreadInfo();

private:
    static std::thread::id main_thread_id_;
    static std::thread::id render_thread_id_;
};
} // namespace sparkle
