#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/ConfigManager.h"
#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/ThreadManager.h"
#include "core/cook/CookArtifactStore.h"
#include "core/cook/Cooker.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

namespace sparkle
{
class TestCookJob final : public CookJob
{
public:
    TestCookJob(CookArtifactKey key, std::atomic<int> *execute_count, const std::atomic<bool> *release_gate = nullptr,
                CookPayload payload = {'u', 'n', 'e', 'x', 'p'}, bool report_progress = false)
        : key_(std::move(key)), execute_count_(execute_count), release_gate_(release_gate),
          payload_(std::move(payload)), report_progress_(report_progress)
    {
        ASSERT(key_.source_hash.has_value());
    }

    [[nodiscard]] const char *GetType() const override
    {
        return key_.type.c_str();
    }

    [[nodiscard]] uint32_t GetVersion() const override
    {
        return key_.version;
    }

    [[nodiscard]] std::string GetSourceName() const override
    {
        return key_.source_name;
    }

    [[nodiscard]] uint32_t GetSourceHash() const override
    {
        return key_.source_hash.value_or(0);
    }

    [[nodiscard]] CookJobResult Execute() override
    {
        execute_count_->fetch_add(1, std::memory_order_acq_rel);
        while (release_gate_ != nullptr && !release_gate_->load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return CookJobResult::Success(payload_);
    }

    [[nodiscard]] float GetProgress() const override
    {
        return report_progress_ ? 0.5f : -1.f;
    }

private:
    CookArtifactKey key_;
    std::atomic<int> *execute_count_;
    const std::atomic<bool> *release_gate_;
    CookPayload payload_;
    bool report_progress_;
};

class CookerRequestTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        switch (stage_)
        {
        case Stage::SeedExactHit:
            return SeedExactHit();
        case Stage::WaitExactHit:
            return WaitExactHit();
        case Stage::RequestContentAlias:
            return RequestContentAlias();
        case Stage::WaitContentAlias:
            return WaitContentAlias();
        case Stage::RequestResolvedZeroMismatch:
            return RequestResolvedZeroMismatch();
        case Stage::WaitResolvedZeroMismatch:
            return WaitResolvedZeroMismatch();
        case Stage::RequestDuplicate:
            return RequestDuplicate();
        case Stage::WaitDuplicate:
            return WaitDuplicate();
        case Stage::RequestDistinctProgress:
            return RequestDistinctProgress();
        case Stage::ReleaseDistinctProgress:
            return ReleaseDistinctProgress();
        case Stage::WaitDistinctProgress:
            return WaitDistinctProgress();
        case Stage::RebuildCacheScope:
            return RebuildCacheScope();
        case Stage::WaitForRenderer:
            return app.GetRenderFramework()->IsReadyForAutoScreenshot() ? Result::Pass : Result::Pending;
        default:
            return Result::Fail;
        }
    }

private:
    enum class Stage : uint8_t
    {
        SeedExactHit,
        WaitExactHit,
        RequestContentAlias,
        WaitContentAlias,
        RequestResolvedZeroMismatch,
        WaitResolvedZeroMismatch,
        RequestDuplicate,
        WaitDuplicate,
        RequestDistinctProgress,
        ReleaseDistinctProgress,
        WaitDistinctProgress,
        RebuildCacheScope,
        WaitForRenderer,
    };

    Result SeedExactHit()
    {
        if (!CookArtifactStore::Save(StoredKey, ExpectedPayload))
        {
            Log(Error, "{}: FAILED - could not seed artifact", GetName());
            return Result::Fail;
        }

        CookArtifactKey lookup_key = StoredKey;
        lookup_key.source_hash.reset();
        handle_ = Cooker::Request(
            lookup_key,
            [this]() -> std::shared_ptr<CookJob> {
                factory_called_.store(true, std::memory_order_release);
                return nullptr;
            },
            [this](CookResult result) { RecordResult(std::move(result)); });

        stage_ = Stage::WaitExactHit;
        return Result::Pending;
    }

    Result WaitExactHit()
    {
        if (!handle_.OnDelivered()->IsReady())
        {
            return Result::Pending;
        }

        bool success = true;
        success &= Expect(!factory_called_.load(std::memory_order_acquire),
                          "cache hit does not construct the source-dependent job");
        success &= Expect(callback_on_main_thread_, "cache hit is delivered on the main thread");
        success &= Expect(callback_succeeded_, "cache hit reports success");
        success &= Expect(received_payload_ == ExpectedPayload, "cache hit returns the stored payload");

        stage_ = Stage::RequestContentAlias;
        return success ? Result::Pending : Result::Fail;
    }

    Result RequestContentAlias()
    {
        ResetResult();

        CookArtifactKey alias_key = StoredKey;
        alias_key.source_name = "exports/textures/contract.bin";

        CookArtifactKey lookup_key = alias_key;
        lookup_key.source_hash.reset();

        handle_ = Cooker::Request(
            lookup_key,
            [this, alias_key]() -> std::shared_ptr<CookJob> {
                factory_called_.store(true, std::memory_order_release);
                return std::make_shared<TestCookJob>(alias_key, &job_execute_count_);
            },
            [this](CookResult result) { RecordResult(std::move(result)); });

        stage_ = Stage::WaitContentAlias;
        return Result::Pending;
    }

    Result WaitContentAlias()
    {
        if (!handle_.OnDelivered()->IsReady())
        {
            return Result::Pending;
        }

        bool success = true;
        success &= Expect(factory_called_.load(std::memory_order_acquire),
                          "relocated source constructs a job to discover its content hash");
        success &= Expect(job_execute_count_.load(std::memory_order_acquire) == 0,
                          "matching content reuses the artifact without recooking");
        success &= Expect(callback_on_main_thread_, "content-alias hit is delivered on the main thread");
        success &= Expect(callback_succeeded_, "content-alias hit reports success");
        success &= Expect(received_payload_ == ExpectedPayload, "content-alias hit returns the stored payload");

        stage_ = Stage::RequestResolvedZeroMismatch;
        return success ? Result::Pending : Result::Fail;
    }

    Result RequestResolvedZeroMismatch()
    {
        ResetResult();

        CookArtifactKey job_key = ResolvedZeroKey;
        job_key.source_hash = 1;

        handle_ = Cooker::Request(
            ResolvedZeroKey,
            [this, job_key]() -> std::shared_ptr<CookJob> {
                factory_called_.store(true, std::memory_order_release);
                return std::make_shared<TestCookJob>(job_key, &job_execute_count_);
            },
            [this](CookResult result) { RecordResult(std::move(result)); });

        stage_ = Stage::WaitResolvedZeroMismatch;
        return Result::Pending;
    }

    Result WaitResolvedZeroMismatch()
    {
        if (!handle_.OnDelivered()->IsReady())
        {
            return Result::Pending;
        }

        bool success = true;
        success &= Expect(factory_called_.load(std::memory_order_acquire),
                          "resolved zero hash is not treated as a logical cache lookup");
        success &= Expect(job_execute_count_.load(std::memory_order_acquire) == 0,
                          "resolved zero hash rejects a job with different content");
        success &= Expect(!callback_succeeded_, "identity mismatch reports failure");
        success &= Expect(received_payload_.empty(), "identity mismatch returns no payload");

        stage_ = Stage::RequestDuplicate;
        return success ? Result::Pending : Result::Fail;
    }

    Result RequestDuplicate()
    {
        ResetResult();
        duplicate_release_.store(false);
        duplicate_delivery_count_ = 0;
        duplicate_all_match_ = true;

        // drop artifacts of previous runs so both requests miss and race for execution
        auto *file_manager = FileManager::GetNativeFileManager();
        std::filesystem::remove_all(
            file_manager->ResolvePath(Path::Internal(std::string("cooked/") + DuplicateKey.type)));

        auto record = [this](CookResult result) {
            duplicate_delivery_count_++;
            duplicate_all_match_ &= result.IsSuccess() && result.payload == DuplicatePayload &&
                                    ThreadManager::IsInCurrentThread(ThreadName::Main);
        };

        handle_ = Cooker::Request(
            std::make_unique<TestCookJob>(DuplicateKey, &job_execute_count_, &duplicate_release_, DuplicatePayload),
            record);
        duplicate_handle_ = Cooker::Request(
            std::make_unique<TestCookJob>(DuplicateKey, &job_execute_count_, &duplicate_release_, DuplicatePayload),
            record);

        duplicate_release_.store(true);

        stage_ = Stage::WaitDuplicate;
        return Result::Pending;
    }

    Result WaitDuplicate()
    {
        if (!handle_.OnDelivered()->IsReady() || !duplicate_handle_.OnDelivered()->IsReady())
        {
            return Result::Pending;
        }

        bool success = true;
        success &= Expect(job_execute_count_.load(std::memory_order_acquire) == 1,
                          "concurrent identical requests execute the job once");
        success &= Expect(duplicate_delivery_count_ == 2, "both requesters receive a delivery");
        success &= Expect(duplicate_all_match_, "both deliveries succeed with the shared payload on the main thread");

        stage_ = Stage::RequestDistinctProgress;
        return success ? Result::Pending : Result::Fail;
    }

    Result RequestDistinctProgress()
    {
        distinct_release_.store(false, std::memory_order_release);
        distinct_execute_count_.store(0, std::memory_order_release);
        distinct_delivery_count_ = 0;
        distinct_all_completed_ = true;
        distinct_progress_ticks_ = 0;

        auto *file_manager = FileManager::GetNativeFileManager();
        std::filesystem::remove_all(
            file_manager->ResolvePath(Path::Internal(std::string("cooked/") + DistinctProgressKeyA.type)));

        auto record = [this](CookResult result) {
            distinct_delivery_count_++;
            distinct_all_completed_ &= result.status == CookResult::Status::ExecutionFailed &&
                                       ThreadManager::IsInCurrentThread(ThreadName::Main);
        };

        distinct_handle_a_ =
            Cooker::Request(std::make_unique<TestCookJob>(DistinctProgressKeyA, &distinct_execute_count_,
                                                          &distinct_release_, CookPayload{}, true),
                            record);
        distinct_handle_b_ =
            Cooker::Request(std::make_unique<TestCookJob>(DistinctProgressKeyB, &distinct_execute_count_,
                                                          &distinct_release_, CookPayload{}, true),
                            record);

        stage_ = Stage::ReleaseDistinctProgress;
        return Result::Pending;
    }

    Result ReleaseDistinctProgress()
    {
        if (distinct_execute_count_.load(std::memory_order_acquire) != 2)
        {
            return Result::Pending;
        }

        if (distinct_progress_ticks_++ < 2)
        {
            return Result::Pending;
        }

        distinct_release_.store(true, std::memory_order_release);
        stage_ = Stage::WaitDistinctProgress;
        return Result::Pending;
    }

    Result WaitDistinctProgress()
    {
        if (!distinct_handle_a_.OnDelivered()->IsReady() || !distinct_handle_b_.OnDelivered()->IsReady())
        {
            return Result::Pending;
        }

        bool success = true;
        success &= Expect(distinct_execute_count_.load(std::memory_order_acquire) == 2,
                          "same-name requests with distinct hashes execute independently");
        success &= Expect(distinct_delivery_count_ == 2, "both distinct-hash requesters receive a delivery");
        success &=
            Expect(distinct_all_completed_, "distinct-hash progress entries complete independently on the main thread");

        stage_ = Stage::RebuildCacheScope;
        return success ? Result::Pending : Result::Fail;
    }

    Result RebuildCacheScope()
    {
        if (!CookArtifactStore::Save(FallbackKey, ExpectedPayload))
        {
            Log(Error, "{}: FAILED - could not seed artifact", GetName());
            return Result::Fail;
        }

        auto *rebuild_config = ConfigManager::Instance().GetConfig<bool>("rebuild_cache");
        if (rebuild_config == nullptr)
        {
            Log(Error, "{}: FAILED - rebuild_cache config not found", GetName());
            return Result::Fail;
        }

        CookArtifactKey lookup_key = FallbackKey;
        lookup_key.source_hash.reset();

        const bool previous = rebuild_config->Get();
        job_execute_count_.store(0, std::memory_order_release);
        rebuild_config->Set(true);

        auto rebuilt = Cooker::CookNow(lookup_key, [this]() {
            return std::make_shared<TestCookJob>(FallbackKey, &job_execute_count_, nullptr, RebuiltPayload);
        });
        auto unavailable = Cooker::CookNow(lookup_key, []() -> std::shared_ptr<CookJob> { return nullptr; });

        rebuild_config->Set(previous);

        bool success = true;
        success &= Expect(job_execute_count_.load(std::memory_order_acquire) == 1,
                          "rebuild_cache re-executes a source-backed job");
        success &= Expect(rebuilt.status == CookResult::Status::Ready && rebuilt.payload == RebuiltPayload,
                          "rebuild_cache returns the rebuilt payload");
        success &= Expect(unavailable.status == CookResult::Status::JobUnavailable && unavailable.payload.empty(),
                          "rebuild_cache does not resurrect an internal artifact without a source");

        stage_ = Stage::WaitForRenderer;
        return success ? Result::Pending : Result::Fail;
    }

    void RecordResult(CookResult result)
    {
        callback_on_main_thread_ = ThreadManager::IsInCurrentThread(ThreadName::Main);
        callback_succeeded_ = result.IsSuccess();
        received_payload_ = std::move(result.payload);
    }

    void ResetResult()
    {
        factory_called_.store(false, std::memory_order_release);
        job_execute_count_.store(0, std::memory_order_release);
        callback_on_main_thread_ = false;
        callback_succeeded_ = false;
        received_payload_.clear();
    }

    bool Expect(bool condition, const char *what) const
    {
        if (condition)
        {
            Log(Info, "{}: OK - {}", GetName(), what);
        }
        else
        {
            Log(Error, "{}: FAILED - {}", GetName(), what);
        }
        return condition;
    }

    static constexpr uint32_t ExpectedSourceHash = 0x4f37a2c1;
    inline static const CookArtifactKey StoredKey{.type = "cooker_request_test",
                                                  .version = 1,
                                                  .source_name = "assets/original/contract.bin",
                                                  .source_hash = ExpectedSourceHash};
    inline static const CookArtifactKey ResolvedZeroKey{.type = "cooker_request_test_zero",
                                                        .version = 1,
                                                        .source_name = "assets/resolved_zero/contract.bin",
                                                        .source_hash = 0};
    inline static const CookArtifactKey DuplicateKey{.type = "cooker_request_test_dup",
                                                     .version = 1,
                                                     .source_name = "assets/duplicate/contract.bin",
                                                     .source_hash = 0x77aa55cc};
    inline static const CookArtifactKey FallbackKey{.type = "cooker_request_test_fallback",
                                                    .version = 1,
                                                    .source_name = "assets/fallback/contract.bin",
                                                    .source_hash = 0x19d2c3b4};
    inline static const CookArtifactKey DistinctProgressKeyA{.type = "cooker_request_test_progress",
                                                             .version = 1,
                                                             .source_name = "assets/progress/contract.bin",
                                                             .source_hash = 0x11223344};
    inline static const CookArtifactKey DistinctProgressKeyB{.type = "cooker_request_test_progress",
                                                             .version = 1,
                                                             .source_name = "assets/progress/contract.bin",
                                                             .source_hash = 0x55667788};
    inline static const std::vector<char> ExpectedPayload{'c', 'o', 'o', 'k', 'e', 'd'};
    inline static const CookPayload DuplicatePayload{'s', 'h', 'a', 'r', 'e', 'd'};
    inline static const CookPayload RebuiltPayload{'r', 'e', 'b', 'u', 'i', 'l', 't'};

    Stage stage_ = Stage::SeedExactHit;
    bool callback_on_main_thread_ = false;
    bool callback_succeeded_ = false;
    std::atomic<bool> factory_called_{false};
    std::atomic<int> job_execute_count_{0};
    std::atomic<bool> duplicate_release_{false};
    int duplicate_delivery_count_ = 0;
    bool duplicate_all_match_ = true;
    std::atomic<bool> distinct_release_{false};
    std::atomic<int> distinct_execute_count_{0};
    int distinct_delivery_count_ = 0;
    int distinct_progress_ticks_ = 0;
    bool distinct_all_completed_ = true;
    std::vector<char> received_payload_;
    CookHandle handle_;
    CookHandle duplicate_handle_;
    CookHandle distinct_handle_a_;
    CookHandle distinct_handle_b_;
};

static TestCaseRegistrar<CookerRequestTest> cooker_request_test_registrar("cooker_request");
} // namespace sparkle
