#pragma once

#include "core/Exception.h"
#include "core/ThreadManager.h"

#include <BS_thread_pool.hpp>

namespace sparkle
{
class TaskManager
{
public:
    explicit TaskManager(unsigned int max_task_threads);

    ~TaskManager()
    {
        is_valid_ = false;
        ConsumeThreadTasks(ThreadName::Worker);
    }

    static std::unique_ptr<TaskManager> CreateInstance(unsigned int max_task_threads);

    static TaskManager &Instance()
    {
        ASSERT_F(instance_, "TaskManager is not initialized. Do not use it before AppFramework::InitCore");
        return *instance_;
    }

    std::future<void> DispatchTask(std::function<void()> task, ThreadName thread_name)
    {
        switch (thread_name)
        {
        case ThreadName::Main:
            return main_thread_tasks_.AddTask(std::move(task));
        case ThreadName::Render:
            return render_thread_tasks_.AddTask(std::move(task));
        case ThreadName::Worker:
            return worker_thread_pool_->submit_task(std::move(task));
        }
    }

    void ConsumeThreadTasks(ThreadName thread_name);

    std::vector<std::function<void()>> PopRenderThreadTasks()
    {
        return render_thread_tasks_.PopTasks();
    }

    static auto RunInMainThread(std::function<void()> task)
    {
        if (ThreadManager::IsInMainThread())
        {
            task();
            auto task_promise = std::make_shared<std::promise<void>>();
            task_promise->set_value();
            return task_promise->get_future();
        }

        return TaskManager::Instance().DispatchTask(std::move(task), ThreadName::Main);
    }

    static auto RunInRenderThread(std::function<void()> task)
    {
        if (ThreadManager::IsRenderThreadRunning() && ThreadManager::IsInRenderThread())
        {
            // run in current thread
            task();
            auto task_promise = std::make_shared<std::promise<void>>();
            task_promise->set_value();
            return task_promise->get_future();
        }

        return TaskManager::Instance().DispatchTask(std::move(task), ThreadName::Render);
    }

    static auto RunInWorkerThread(std::function<void()> task)
    {
        return TaskManager::Instance().DispatchTask(std::move(task), ThreadName::Worker);
    }

    template <typename F> static auto ParallelFor(unsigned first_index, unsigned index_after_last, F &&task)
    {
        return TaskManager::Instance().GetThreadPool().submit_loop(first_index, index_after_last, task);
    }

private:
    BS::light_thread_pool &GetThreadPool()
    {
        ASSERT(is_valid_);
        return *worker_thread_pool_;
    }

    struct ThreadTaskQueue
    {
        std::vector<std::function<void()>> tasks;
        std::mutex mutex;

        std::future<void> AddTask(std::function<void()> task);

        void RunAll();

        std::vector<std::function<void()>> PopTasks();
    };

    std::unique_ptr<BS::light_thread_pool> worker_thread_pool_;

    ThreadTaskQueue main_thread_tasks_;
    ThreadTaskQueue render_thread_tasks_;

    bool is_valid_ = true;

    static TaskManager *instance_;
};

} // namespace sparkle
