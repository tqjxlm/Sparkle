#pragma once

#include "core/ThreadManager.h"

#include <BS_thread_pool.hpp>

#include <condition_variable>
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
// It uses a monitor thread to wait for new tasks from TaskManager and TaskFuture.
class TaskDispatcher
{
    struct PendingTask
    {
        std::function<void()> task;
        ThreadName thread;
    };

public:
    static TaskDispatcher &Instance()
    {
        return *instance_;
    }

    explicit TaskDispatcher(unsigned int max_parallism);

    ~TaskDispatcher();

    void RegisterTaskQueue(std::weak_ptr<ThreadTaskQueue> task_queue, ThreadName thread)
    {
        task_queues_[thread] = std::move(task_queue);
    }

    void EnqueueTask(std::function<void()> &&task, ThreadName thread_name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_tasks_.emplace(std::move(task), thread_name);
        new_task_pushed_.notify_all();
    }

    BS::light_thread_pool &GetThreadPool()
    {
        return *worker_thread_pool_;
    }

    // Flush all pending tasks destined for the given thread directly to its
    // queue.  Call from the producer thread right before consuming the queue
    // to eliminate the one-frame latency introduced by the monitor thread.
    void FlushPendingTasks(ThreadName target_thread)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto queue = task_queues_[target_thread].lock();
        if (!queue)
        {
            return;
        }

        std::queue<PendingTask> remaining;
        while (!pending_tasks_.empty())
        {
            auto pending = std::move(pending_tasks_.front());
            pending_tasks_.pop();
            if (pending.thread == target_thread)
            {
                queue->AddTask(std::move(pending.task));
            }
            else
            {
                remaining.push(std::move(pending));
            }
        }
        pending_tasks_ = std::move(remaining);
    }

private:
    void DispatchPendingTasks();

    std::queue<PendingTask> pending_tasks_;

    std::mutex mutex_;

    std::unique_ptr<BS::light_thread_pool> worker_thread_pool_;

    std::unordered_map<ThreadName, std::weak_ptr<ThreadTaskQueue>> task_queues_;

    std::thread monitor_thread_;

    std::condition_variable new_task_pushed_;

    static TaskDispatcher *instance_;

    std::atomic<bool> shutdown_requested_{false};
};
} // namespace sparkle
