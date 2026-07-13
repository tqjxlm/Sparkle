#pragma once

#include "core/Exception.h"
#include "core/ThreadManager.h"
#include "core/task/TaskDispatcher.h"
#include "core/task/TaskFuture.h"

namespace sparkle
{
// A typical async task life cycle:
// 1. call TaskManager::EnqueueTask from any thread.
// 2. the task is handed over to TaskDispatcher where it is dispatched to the target thread.
// 3. EnqueueTask returns a future to which you can attach callbacks.
class TaskManager
{
public:
    explicit TaskManager(unsigned int max_parallism);

    ~TaskManager()
    {
        // join worker threads before dropping the singleton: in-flight tasks (e.g. cook
        // jobs) may still call Instance() while they finish
        task_dispatcher_.reset();
        instance_ = nullptr;
    }

    static TaskManager &Instance()
    {
        ASSERT_F(instance_, "TaskManager is not initialized. Do not use it before AppFramework::InitCore");
        return *instance_;
    }

    template <std::invocable<> Func>
    auto EnqueueTask(Func &&task, TargetThread target_thread, bool allow_run_now = true)
    {
        using ReturnType = std::invoke_result_t<Func>;

        auto task_promise = std::make_shared<std::promise<ReturnType>>();
        auto future = std::make_shared<TaskFuture<ReturnType>>(task_promise->get_future());

        auto task_to_dispatch = PackageTask<ReturnType>(std::forward<Func>(task), task_promise, future);

        ThreadName thread_name = GetTargetThreadName(target_thread);

        // it is possible the caller is already in the target thread
        bool run_now = allow_run_now && ThreadManager::IsInCurrentThread(thread_name);
        if (run_now)
        {
            task_to_dispatch();
        }
        else
        {
            TaskDispatcher::Instance().EnqueueTask(std::move(task_to_dispatch), thread_name);
        }

        return future;
    }

    template <std::invocable<> Func> static auto RunInMainThread(Func &&task, bool allow_run_now = true)
    {
        return TaskManager::Instance().EnqueueTask(std::forward<Func>(task), TargetThread::Main, allow_run_now);
    }

    template <std::invocable<> Func> static auto RunInRenderThread(Func &&task, bool allow_run_now = true)
    {
        return TaskManager::Instance().EnqueueTask(std::forward<Func>(task), TargetThread::Render, allow_run_now);
    }

    template <std::invocable<> Func> static auto RunInWorkerThread(Func &&task, bool allow_run_now = true)
    {
        return TaskManager::Instance().EnqueueTask(std::forward<Func>(task), TargetThread::Worker, allow_run_now);
    }

    // for coordinator tasks that fan out to the worker pool and block on it (ParallelFor, OnAll):
    // a pool worker must never host such a task, or pools with few threads deadlock
    template <std::invocable<> Func> static auto RunInDedicatedThread(Func &&task)
    {
        using ReturnType = std::invoke_result_t<Func>;

        auto task_promise = std::make_shared<std::promise<ReturnType>>();
        auto future = std::make_shared<TaskFuture<ReturnType>>(task_promise->get_future());

        TaskDispatcher::Instance().RunInDedicatedThread(
            PackageTask<ReturnType>(std::forward<Func>(task), task_promise, future));

        return future;
    }

    template <typename Func> static auto ParallelFor(unsigned first_index, unsigned index_after_last, Func &&task)
    {
        return TaskDispatcher::Instance().GetThreadPool().submit_loop(first_index, index_after_last,
                                                                      std::forward<Func>(task));
    }

    static std::shared_ptr<TaskFuture<>> OnAll(const std::vector<std::shared_ptr<TaskFuture<>>> &tasks);

private:
    template <typename ReturnType, typename Func>
    static std::function<void()> PackageTask(Func &&task, std::shared_ptr<std::promise<ReturnType>> task_promise,
                                             std::shared_ptr<TaskFuture<ReturnType>> future)
    {
        return [async_task = std::function<ReturnType()>(std::forward<Func>(task)), future,
                promise = std::move(task_promise)]() {
            if constexpr (std::is_void_v<ReturnType>)
            {
                async_task();
                promise->set_value();
            }
            else
            {
                promise->set_value(async_task());
            }
            future->OnReady();
        };
    }

    std::unique_ptr<TaskDispatcher> task_dispatcher_;

    static TaskManager *instance_;
};
} // namespace sparkle
