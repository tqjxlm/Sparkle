#pragma once

#include "core/ThreadManager.h"
#include "core/task/TaskDispatcher.h"

#include <concepts>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

namespace sparkle
{
enum class TargetThread : uint8_t
{
    Current, // the thread where Then() is called
    Main,
    Render,
    Worker,
};

inline ThreadName GetTargetThreadName(TargetThread target_thread)
{
    switch (target_thread)
    {
    case TargetThread::Current:
        return ThreadManager::CurrentThread();
    case TargetThread::Main:
        return ThreadName::Main;
    case TargetThread::Render:
        return ThreadName::Render;
    case TargetThread::Worker:
        return ThreadName::Worker;
    default:
        UnImplemented(target_thread);
        return ThreadName::Main;
    }
}

// A future is created along with a task to provide its completion status.
// You can use it to chain tasks together.
// The future is not bound to tasks, so it is safe to use even after the task is destroyed.
// It is not suitable for heavy use due to cross-thread safety measurements.
template <typename ReturnType = void>
    requires std::movable<ReturnType> || std::is_void_v<ReturnType>
class TaskFuture
{
public:
    explicit TaskFuture(std::future<ReturnType> &&future) : future_(std::move(future).share())
    {
    }

    explicit TaskFuture(std::shared_future<ReturnType> &&future) : future_(std::move(future))
    {
    }

    TaskFuture(const TaskFuture &) = delete;
    TaskFuture &operator=(const TaskFuture &) = delete;
    TaskFuture(TaskFuture &&) = default;
    TaskFuture &operator=(TaskFuture &&) = default;
    ~TaskFuture() = default;

    template <std::invocable<> Func>
        requires std::is_void_v<ReturnType>
    auto Then(Func &&callback, TargetThread thread = TargetThread::Current)
    {
        using ThenReturnType = std::invoke_result_t<Func>;
        std::function<ThenReturnType()> task = std::forward<Func>(callback);
        ThreadName thread_name = GetTargetThreadName(thread);

        auto then_promise = std::make_shared<std::promise<ThenReturnType>>();
        auto then_future = std::make_shared<TaskFuture<ThenReturnType>>(then_promise->get_future());

        auto then_task = [func = std::move(task), then_future, then_promise]() {
            if constexpr (std::is_void_v<ThenReturnType>)
            {
                func();
                then_promise->set_value();
            }
            else
            {
                then_promise->set_value(func());
            }
            then_future->OnReady();
        };

        DispatchOrEnqueueTask(std::move(then_task), thread_name);
        return then_future;
    }

    template <std::invocable<ReturnType> Func>
        requires(!std::is_void_v<ReturnType>)
    auto Then(Func &&callback, TargetThread target_thread = TargetThread::Current)
    {
        using ThenReturnType = std::invoke_result_t<Func, ReturnType>;
        std::function<ThenReturnType(ReturnType)> task = std::forward<Func>(callback);

        ThreadName thread_name = GetTargetThreadName(target_thread);

        auto then_promise = std::make_shared<std::promise<ThenReturnType>>();
        auto then_future =
            std::shared_ptr<TaskFuture<ThenReturnType>>(new TaskFuture<ThenReturnType>(then_promise->get_future()));

        auto then_task = [func = std::move(task), then_future, then_promise](ReturnType arg) {
            if constexpr (std::is_void_v<ThenReturnType>)
            {
                func(arg);
                then_promise->set_value();
            }
            else
            {
                then_promise->set_value(func(arg));
            }
            then_future->OnReady();
        };

        DispatchOrEnqueueTask(std::move(then_task), thread_name);
        return then_future;
    }

    void OnReady()
    {
        ASSERT(IsReady());

        std::lock_guard<std::mutex> lock(mutex_);

        if constexpr (std::is_void_v<ReturnType>)
        {
            for (auto &[call_back_task, thread] : callbacks_)
            {
                TaskDispatcher::Instance().EnqueueTask(std::move(call_back_task), thread);
            }
        }
        else
        {
            auto value = future_.get();
            for (auto &[call_back_task, thread] : callbacks_)
            {
                TaskDispatcher::Instance().EnqueueTask([value, task = std::move(call_back_task)]() { task(value); },
                                                       thread);
            }
        }
        callbacks_.clear();
    }

    void Wait()
    {
        future_.wait();
    }

    [[nodiscard]] bool IsReady() const
    {
        return future_.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready;
    }

private:
    template <typename TaskFunc> void DispatchOrEnqueueTask(TaskFunc &&task, ThreadName thread_name)
    {
        if (!IsReady())
        {
            // if current future is not ready, enqueue the task to be executed when it is ready.
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_.emplace_back(std::forward<TaskFunc>(task), thread_name);
            return;
        }

        // if ready, dispatch its callbacks to the target thread
        if (ThreadManager::IsInCurrentThread(thread_name))
        {
            if constexpr (std::is_void_v<ReturnType>)
            {
                task();
            }
            else
            {
                task(future_.get());
            }
        }
        else
        {
            if constexpr (std::is_void_v<ReturnType>)
            {
                TaskDispatcher::Instance().EnqueueTask(std::forward<TaskFunc>(task), thread_name);
            }
            else
            {
                auto value = future_.get();
                TaskDispatcher::Instance().EnqueueTask([value, task = std::forward<TaskFunc>(task)]() { task(value); },
                                                       thread_name);
            }
        }
    }

    std::shared_future<ReturnType> future_;

    struct FutureCallback
    {
        std::function<void(ReturnType)> task;
        ThreadName thread;
    };

    struct VoidFutureCallback
    {
        std::function<void()> task;
        ThreadName thread;
    };

    std::conditional_t<std::is_void_v<ReturnType>, std::vector<VoidFutureCallback>, std::vector<FutureCallback>>
        callbacks_;

    std::mutex mutex_;
};

} // namespace sparkle