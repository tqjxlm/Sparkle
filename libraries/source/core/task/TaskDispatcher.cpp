#include "core/task/TaskDispatcher.h"

#include "core/Logger.h"

namespace sparkle
{
TaskDispatcher *TaskDispatcher::instance_ = nullptr;

TaskDispatcher::TaskDispatcher(unsigned int max_parallism)
{
    ASSERT(instance_ == nullptr);
    instance_ = this;

    // reserve 2 threads for main and render
    unsigned max_task_threads = std::min(max_parallism, std::thread::hardware_concurrency()) - 2;
    ASSERT(max_task_threads > 0);

    worker_thread_pool_ = std::make_unique<BS::light_thread_pool>(
        max_task_threads, [](std::size_t idx) { ThreadManager::RegisterTaskThread(idx); });

    Log(Info, "num threads in thread pool: {}", worker_thread_pool_->get_thread_count());

    monitor_thread_ = std::thread([this]() {
        while (!shutdown_requested_)
        {
            DispatchPendingTasks();
        }
    });
}

TaskDispatcher::~TaskDispatcher()
{
    instance_ = nullptr;

    shutdown_requested_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        new_task_pushed_.notify_all();
    }

    if (monitor_thread_.joinable())
    {
        monitor_thread_.join();
    }
}

void TaskDispatcher::DispatchPendingTasks()
{
    // when there's no pending task, release thread resources and wait
    std::unique_lock<std::mutex> lock(mutex_);
    new_task_pushed_.wait(lock, [this] { return !pending_tasks_.empty() || shutdown_requested_; });

    while (!pending_tasks_.empty() && !shutdown_requested_)
    {
        auto [task, thread_name] = pending_tasks_.front();
        pending_tasks_.pop();

        if (thread_name == ThreadName::Worker)
        {
            // for worker thread task, hand it over to thread pool and forget it
            worker_thread_pool_->submit_task(std::move(task)).wait_for(std::chrono::nanoseconds(0));
        }
        else
        {
            // for other named thread task, leave it to the thread to consume
            auto task_queue = task_queues_[thread_name].lock();
            if (task_queue)
            {
                task_queue->AddTask(std::move(task));
            }
        }
    }
}

std::vector<std::function<void()>> ThreadTaskQueue::PopTasks()
{
    std::vector<std::function<void()>> tasks_copy;
    {
        std::lock_guard<std::mutex> lock(mutex);
        tasks_copy.swap(tasks);
    }
    return tasks_copy;
}

void ThreadTaskQueue::RunAll()
{
    auto tasks_copy = PopTasks();

    for (auto &task : tasks_copy)
    {
        task();
    }
}

std::future<void> ThreadTaskQueue::AddTask(std::function<void()> &&task)
{
    auto task_promise = std::make_shared<std::promise<void>>();

    std::lock_guard<std::mutex> lock(mutex);
    tasks.emplace_back([task_moved = std::move(task), task_promise] {
        task_moved();
        task_promise->set_value();
    });
    return task_promise->get_future();
}
} // namespace sparkle
