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
        instance_ = nullptr;
    }

    static TaskManager &Instance()
    {
        ASSERT_F(instance_, "TaskManager is not initialized. Do not use it before AppFramework::InitCore");
        return *instance_;
    }

    template <std::invocable<> Func> auto EnqueueTask(Func &&task, TargetThread target_thread)
    {
        using ReturnType = std::invoke_result_t<Func>;
        std::function<ReturnType()> func = std::forward<Func>(task);

        auto task_promise = std::make_shared<std::promise<ReturnType>>();
        auto future = std::make_shared<TaskFuture<ReturnType>>(task_promise->get_future());

        auto task_to_dispatch = [async_task = std::move(func), future, task_promise]() {
            if constexpr (std::is_void_v<ReturnType>)
            {
                async_task();
                task_promise->set_value();
            }
            else
            {
                task_promise->set_value(async_task());
            }
            future->OnReady();
        };

        ThreadName thread_name = GetTargetThreadName(target_thread);

        // it is possible the caller is already in the target thread
        bool run_now = ThreadManager::IsInCurrentThread(thread_name);
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

    template <std::invocable<> Func> static auto RunInMainThread(Func &&task)
    {
        return TaskManager::Instance().EnqueueTask(std::forward<Func>(task), TargetThread::Main);
    }

    template <std::invocable<> Func> static auto RunInRenderThread(Func &&task)
    {
        return TaskManager::Instance().EnqueueTask(std::forward<Func>(task), TargetThread::Render);
    }

    template <std::invocable<> Func> static auto RunInWorkerThread(Func &&task)
    {
        return TaskManager::Instance().EnqueueTask(std::forward<Func>(task), TargetThread::Worker);
    }

    template <typename Func> static auto ParallelFor(unsigned first_index, unsigned index_after_last, Func &&task)
    {
        return TaskDispatcher::Instance().GetThreadPool().submit_loop(first_index, index_after_last,
                                                                      std::forward<Func>(task));
    }

    static std::shared_ptr<TaskFuture<>> OnAll(const std::vector<std::shared_ptr<TaskFuture<>>> &tasks);

private:
    std::unique_ptr<TaskDispatcher> task_dispatcher_;

    static TaskManager *instance_;
};
} // namespace sparkle
