#include "core/task/TaskManager.h"

#include "core/Exception.h"

namespace sparkle
{
TaskManager *TaskManager::instance_ = nullptr;

TaskManager::TaskManager(unsigned int max_parallism, unsigned int reserved_threads)
{
    ASSERT(instance_ == nullptr);
    instance_ = this;

    task_dispatcher_ = std::make_unique<TaskDispatcher>(max_parallism, reserved_threads);
}

std::shared_ptr<TaskFuture<>> TaskManager::OnAll(const std::vector<std::shared_ptr<TaskFuture<>>> &tasks)
{
    auto promise = std::make_shared<std::promise<void>>();
    auto future = std::make_shared<TaskFuture<>>(promise->get_future());

    if (tasks.empty())
    {
        promise->set_value();
        future->OnReady();
        return future;
    }

    auto counter = std::make_shared<std::atomic<size_t>>(tasks.size());

    for (const auto &task : tasks)
    {
        task->Then(
            [promise, counter, future]() {
                if (counter->fetch_sub(1) == 1)
                {
                    promise->set_value();
                    future->OnReady();
                }
            },
            TargetThread::Worker);
    }

    return future;
}

void TaskManager::FlushRenderThread()
{
    ASSERT(ThreadManager::IsInMainThread());

    // without a thread of its own, a render task runs where it is queued
    if (ThreadManager::IsInRenderThread())
    {
        return;
    }

    auto fence = RunInRenderThread([] {});
    TaskDispatcher::Instance().DeliverQueuedTasks(ThreadName::Render);
    fence->Wait();
}
} // namespace sparkle
