#pragma once

#include "core/ThreadManager.h"

#include <BS_thread_pool.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

namespace sparkle
{
struct ThreadTaskQueue
{
    std::vector<std::function<void()>> tasks;
    std::mutex mutex;

    std::future<void> AddTask(std::function<void()> &&task);

    void RunAll();

    std::vector<std::function<void()>> PopTasks();
};

// Task dispatcher routes tasks to different threads.
// A named thread's task lands in that thread's queue before EnqueueTask returns, so it is ordered with the
// caller's frame. Worker tasks go through a monitor thread that hands them to the thread pool.
class TaskDispatcher
{
public:
    static TaskDispatcher &Instance()
    {
        return *instance_;
    }

    TaskDispatcher(unsigned int max_parallism, unsigned int reserved_threads);

    ~TaskDispatcher();

    void RegisterTaskQueue(std::weak_ptr<ThreadTaskQueue> task_queue, ThreadName thread)
    {
        std::scoped_lock<std::mutex> lock(mutex_);
        task_queues_[thread] = std::move(task_queue);
    }

    void EnqueueTask(std::function<void()> &&task, ThreadName thread_name)
    {
        std::scoped_lock<std::mutex> lock(mutex_);
        if (thread_name != ThreadName::Worker)
        {
            if (auto task_queue = task_queues_[thread_name].lock())
            {
                task_queue->AddTask(std::move(task));
            }
            return;
        }
        pending_tasks_.push(std::move(task));
        new_task_pushed_.notify_all();
    }

    BS::light_thread_pool &GetThreadPool()
    {
        return *worker_thread_pool_;
    }

    void RunInDedicatedThread(std::function<void()> &&task);

    // drains the queue of a named thread; must be called on that thread
    void RunQueuedTasks(ThreadName thread)
    {
        if (auto queue = task_queues_[thread].lock())
        {
            queue->RunAll();
        }
    }

private:
    struct DedicatedThread
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void DispatchPendingTasks();

    std::queue<std::function<void()>> pending_tasks_;

    std::mutex mutex_;

    std::unique_ptr<BS::light_thread_pool> worker_thread_pool_;

    std::vector<DedicatedThread> dedicated_threads_;

    std::mutex dedicated_mutex_;

    std::unordered_map<ThreadName, std::weak_ptr<ThreadTaskQueue>> task_queues_;

    std::thread monitor_thread_;

    std::condition_variable new_task_pushed_;

    static TaskDispatcher *instance_;

    std::atomic<bool> shutdown_requested_{false};
};
} // namespace sparkle
