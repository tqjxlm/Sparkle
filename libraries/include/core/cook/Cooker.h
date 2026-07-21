#pragma once

#include "core/Exception.h"
#include "core/cook/CookJob.h"
#include "core/task/TaskFuture.h"

#include <atomic>
#include <functional>
#include <future>
#include <memory>

namespace sparkle
{
class CookHandle
{
public:
    CookHandle() = default;

    ~CookHandle()
    {
        Cancel();
    }

    CookHandle(const CookHandle &) = delete;
    CookHandle &operator=(const CookHandle &) = delete;

    CookHandle(CookHandle &&other) noexcept = default;

    CookHandle &operator=(CookHandle &&other) noexcept
    {
        if (this != &other)
        {
            Cancel();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    // becomes ready on the main thread once delivery ran (or was cancelled)
    [[nodiscard]] const std::shared_ptr<TaskFuture<>> &OnDelivered() const
    {
        ASSERT(state_);
        return state_->delivered_future;
    }

    [[nodiscard]] bool IsValid() const
    {
        return state_ != nullptr;
    }

    // stops the pending delivery. the job itself may still finish and populate the disk cache
    void Cancel()
    {
        if (state_)
        {
            state_->cancelled.store(true);
            state_ = nullptr;
        }
    }

private:
    struct State
    {
        std::atomic<bool> cancelled{false};
        std::shared_ptr<std::promise<void>> delivered_promise;
        std::shared_ptr<TaskFuture<>> delivered_future;
    };

    explicit CookHandle(std::shared_ptr<State> state) : state_(std::move(state))
    {
    }

    std::shared_ptr<State> state_;

    friend class Cooker;
};

struct CookResult
{
    enum class Status : uint8_t
    {
        Ready,
        JobUnavailable,
        IdentityMismatch,
        ExecutionFailed,
        StoreFailed,
    };

    Status status = Status::ExecutionFailed;
    CookPayload payload;

    [[nodiscard]] bool IsSuccess() const
    {
        return status == Status::Ready;
    }

    [[nodiscard]] bool HasPayload() const
    {
        return !payload.empty();
    }
};

// orchestrates cook jobs: artifact lookup, async execution, disk caching and delivery.
// lookup order: packaged resources (build-time cooks) -> internal storage (previous runtime
// cooks) -> run the job on a dedicated thread and save the artifact to internal storage.
// concurrent requests for the same lookup key share one execution and its delivery.
// RHI-free by design so it stays usable in a render-less cook process.
class Cooker
{
public:
    // on_ready runs on the main thread for cache hits and fresh cooks. A StoreFailed result
    // may still carry a usable payload; destroying the returned handle cancels delivery.
    static CookHandle Request(std::unique_ptr<CookJob> job, std::function<void(CookResult)> on_ready);

    // Resolves an artifact before constructing its source-dependent job. On a miss the
    // factory and job both run off the main thread. This gives cache hits and fresh cooks
    // the same handle, completion, and main-thread delivery contract without loading raw
    // source data merely to perform a lookup.
    using CookJobFactory = std::function<std::shared_ptr<CookJob>()>;

    static CookHandle Request(const CookArtifactKey &lookup_key, CookJobFactory job_factory,
                              std::function<void(CookResult)> on_ready);

    // synchronous counterpart of Request for callers already off the main thread (e.g.
    // scene loading): store hit or inline execution on the calling thread, with the same
    // identity, logging and store contract. no coalescing: concurrent identical requests
    // cook redundantly but save atomically
    static CookResult CookNow(const CookArtifactKey &lookup_key, const CookJobFactory &job_factory);
};
} // namespace sparkle
