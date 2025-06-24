#include "core/TaskManager.h"

#include "core/Exception.h"
#include "core/Logger.h"

namespace sparkle
{
TaskManager *TaskManager::instance_ = nullptr;

TaskManager::TaskManager(unsigned int max_task_threads)
{
    ASSERT(max_task_threads > 0);

    worker_thread_pool_ =
        std::make_unique<BS::light_thread_pool>(std::min(max_task_threads, std::thread::hardware_concurrency()),
                                                [](std::size_t idx) { ThreadManager::RegisterTaskThread(idx); });

    Log(Info, "num threads in thread pool: {}", worker_thread_pool_->get_thread_count());
}

std::unique_ptr<TaskManager> TaskManager::CreateInstance(unsigned int max_task_threads)
{
    ASSERT(instance_ == nullptr);
    auto instance = std::make_unique<TaskManager>(max_task_threads);
    instance_ = instance.get();
    return instance;
}

void TaskManager::ConsumeThreadTasks(ThreadName thread_name)
{
    switch (thread_name)
    {
    case ThreadName::Main:
        ASSERT(ThreadManager::IsInMainThread());
        main_thread_tasks_.RunAll();
        break;
    case ThreadName::Render:
        ASSERT(ThreadManager::IsInRenderThread());
        render_thread_tasks_.RunAll();
        break;
    case ThreadName::Worker:
        worker_thread_pool_->wait();
        break;
    }
}

std::vector<std::function<void()>> TaskManager::ThreadTaskQueue::PopTasks()
{
    std::vector<std::function<void()>> tasks_copy;
    {
        std::lock_guard<std::mutex> lock(mutex);
        tasks_copy.swap(tasks);
    }
    return tasks_copy;
}

void TaskManager::ThreadTaskQueue::RunAll()
{
    std::vector<std::function<void()>> tasks_copy;
    {
        std::lock_guard<std::mutex> lock(mutex);
        tasks_copy.swap(tasks);
    }

    for (auto &task : tasks_copy)
    {
        task();
    }
}

std::future<void> TaskManager::ThreadTaskQueue::AddTask(std::function<void()> task)
{
    auto task_promise = std::make_shared<std::promise<void>>();

    std::lock_guard<std::mutex> lock(mutex);
    tasks.emplace_back([task = std::move(task), task_promise] {
        task();
        task_promise->set_value();
    });
    return task_promise->get_future();
}
} // namespace sparkle
