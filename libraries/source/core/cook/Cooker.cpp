#include "core/cook/Cooker.h"

#include "core/Logger.h"
#include "core/Timer.h"
#include "core/cook/CookArtifactStore.h"
#include "core/task/TaskManager.h"

#include <mutex>
#include <unordered_map>

namespace sparkle
{
namespace
{
void LogProgress(const std::shared_ptr<CookJob> &job, const std::shared_ptr<TaskFuture<>> &delivered)
{
    const auto tag = std::string("Cooker::") + job->GetType();

    if (delivered->IsReady())
    {
        Logger::LogToScreen(tag, "");
        return;
    }

    auto progress = job->GetProgress();
    if (progress >= 0.f)
    {
        Logger::LogToScreen(tag, fmt::format("Cooking {} {:.1f}%", job->GetSourceName(), progress * 100.f));
    }

    TaskManager::RunInMainThread([job, delivered]() { LogProgress(job, delivered); }, false);
}

struct InFlightCook
{
    std::vector<std::function<void(CookResult)>> subscribers;
};

std::mutex &GetInFlightMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, std::shared_ptr<InFlightCook>> &GetInFlightCooks()
{
    static std::unordered_map<std::string, std::shared_ptr<InFlightCook>> cooks;
    return cooks;
}

std::string GetInFlightKey(const CookArtifactKey &key)
{
    return fmt::format("{}:{}:{}:{}", key.type, key.version, key.source_name,
                       key.source_hash ? fmt::format("{:08x}", *key.source_hash) : "?");
}

void FinishInFlight(const std::string &in_flight_key, CookResult result)
{
    std::vector<std::function<void(CookResult)>> subscribers;
    {
        std::scoped_lock<std::mutex> lock(GetInFlightMutex());

        auto &cooks = GetInFlightCooks();
        auto entry = cooks.find(in_flight_key);
        ASSERT(entry != cooks.end());
        subscribers = std::move(entry->second->subscribers);
        cooks.erase(entry);
    }

    ASSERT(!subscribers.empty());
    for (size_t i = 0; i + 1 < subscribers.size(); i++)
    {
        subscribers[i](CookResult{.status = result.status, .payload = result.payload});
    }
    subscribers.back()(std::move(result));
}

CookResult ExecuteAndStore(const CookArtifactKey &lookup_key, const Cooker::CookJobFactory &make_job,
                           const std::shared_ptr<TaskFuture<>> &delivered)
{
    Timer timer;

    auto job = make_job();
    if (!job)
    {
        // a null factory means there is no source to recook from; a packaged artifact
        // is the only copy and must resolve even under rebuild_cache
        if (auto payload = CookArtifactStore::Load(lookup_key, true); !payload.empty())
        {
            return {.status = CookResult::Status::Ready, .payload = std::move(payload)};
        }

        Log(Error, "cannot cook {}: {}: source job creation failed", lookup_key.type, lookup_key.source_name);
        return {.status = CookResult::Status::JobUnavailable, .payload = {}};
    }

    const auto key = MakeCookArtifactKey(*job);
    const bool identity_mismatch = key.type != lookup_key.type || key.version != lookup_key.version ||
                                   key.source_name != lookup_key.source_name ||
                                   (lookup_key.source_hash && key.source_hash != lookup_key.source_hash);
    if (identity_mismatch)
    {
        Log(Error, "cook job identity does not match lookup key: {}:{}", lookup_key.type, lookup_key.source_name);
        return {.status = CookResult::Status::IdentityMismatch, .payload = {}};
    }

    if (auto payload = CookArtifactStore::Load(key); !payload.empty())
    {
        return {.status = CookResult::Status::Ready, .payload = std::move(payload)};
    }

    Log(Info, "cooking {}: {}", key.type, key.source_name);
    TaskManager::RunInMainThread([job, delivered]() { LogProgress(job, delivered); }, false);

    auto job_result = job->Execute();
    if (!job_result.IsSuccess() || job_result.GetPayload().empty())
    {
        Log(Error, "cook produced no data {}: {}", key.type, key.source_name);
        return {.status = CookResult::Status::ExecutionFailed, .payload = {}};
    }

    auto payload = job_result.TakePayload();
    const bool saved = CookArtifactStore::Save(key, payload);

    Log(Info, "cook finished {}: {}. took {:.2f}s", key.type, key.source_name, timer.ElapsedSecond());

    return {.status = saved ? CookResult::Status::Ready : CookResult::Status::StoreFailed,
            .payload = std::move(payload)};
}
} // namespace

CookResult Cooker::CookNow(const CookArtifactKey &lookup_key, const CookJobFactory &job_factory)
{
    // resolve the logical key first, exactly like Request: a hit must not construct the
    // job, whose source-derived identity would otherwise override the manifest's
    if (auto payload = CookArtifactStore::Load(lookup_key); !payload.empty())
    {
        return {.status = CookResult::Status::Ready, .payload = std::move(payload)};
    }

    auto done_promise = std::make_shared<std::promise<void>>();
    done_promise->set_value();
    auto done = std::make_shared<TaskFuture<>>(done_promise->get_future());

    return ExecuteAndStore(lookup_key, job_factory, done);
}

CookHandle Cooker::Request(std::unique_ptr<CookJob> job, std::function<void(CookResult)> on_ready)
{
    ASSERT(job);

    const auto key = MakeCookArtifactKey(*job);
    std::shared_ptr<CookJob> shared_job = std::move(job);

    return Request(key, [shared_job]() { return shared_job; }, std::move(on_ready));
}

CookHandle Cooker::Request(const CookArtifactKey &lookup_key, CookJobFactory job_factory,
                           std::function<void(CookResult)> on_ready)
{
    auto state = std::make_shared<CookHandle::State>();
    state->delivered_promise = std::make_shared<std::promise<void>>();
    state->delivered_future = std::make_shared<TaskFuture<>>(state->delivered_promise->get_future());

    auto deliver = [state, ready_callback = std::move(on_ready)](CookResult result) {
        // never inline: requesters may call Request mid scene-attach and expect it to return
        // before anything is delivered
        TaskManager::RunInMainThread(
            [state, ready_callback, delivery_result = std::move(result)]() mutable {
                if (!state->cancelled.load())
                {
                    ready_callback(std::move(delivery_result));
                }
                state->delivered_promise->set_value();
                state->delivered_future->OnReady();
            },
            false);
    };

    if (auto payload = CookArtifactStore::Load(lookup_key); !payload.empty())
    {
        deliver({.status = CookResult::Status::Ready, .payload = std::move(payload)});
        return CookHandle(state);
    }

    const auto in_flight_key = GetInFlightKey(lookup_key);
    {
        std::scoped_lock<std::mutex> lock(GetInFlightMutex());

        auto &cooks = GetInFlightCooks();
        if (auto entry = cooks.find(in_flight_key); entry != cooks.end())
        {
            entry->second->subscribers.emplace_back(std::move(deliver));
            return CookHandle(state);
        }

        auto in_flight = std::make_shared<InFlightCook>();
        in_flight->subscribers.emplace_back(std::move(deliver));
        cooks.emplace(in_flight_key, std::move(in_flight));
    }

    TaskManager::RunInDedicatedThread([lookup_key, in_flight_key, make_job = std::move(job_factory),
                                       delivered = state->delivered_future]() {
        FinishInFlight(in_flight_key, ExecuteAndStore(lookup_key, make_job, delivered));
    })->Forget();

    return CookHandle(state);
}

} // namespace sparkle
