#include "core/task/TaskManager.h"

#include "core/Exception.h"

namespace sparkle
{
TaskManager *TaskManager::instance_ = nullptr;

TaskManager::TaskManager(unsigned int max_parallism)
{
    ASSERT(instance_ == nullptr);
    instance_ = this;

    task_dispatcher_ = std::make_unique<TaskDispatcher>(max_parallism);
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
        task->Then([promise, counter, future]() {
            if (counter->fetch_sub(1) == 1)
            {
                promise->set_value();
                future->OnReady();
            }
        });
    }

    return future;
}
} // namespace sparkle
