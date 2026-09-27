#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "renderer/graph/RGTexturePool.h"
#include "rhi/RHI.h"

#include <atomic>

namespace sparkle
{
// switches forward -> gpu or deferred -> forward. the graph texture pool outlives the renderers, so each new renderer
// reuses an image the previous one left (every pipeline ends in the shared post chain's screen) and creates fewer
// images than its graphs hold.
class PipelineSwitchPoolTest : public TestCase
{
    static constexpr uint32_t SettleFrames = 30;

public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("pipeline", std::string("forward"));
    }

    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        if (failed_.load(std::memory_order_acquire))
        {
            return Result::Fail;
        }

        auto *rhi = app.GetRHI();

        switch (phase_)
        {
        case Phase::WaitLoaded:
            if (!app.GetRenderFramework()->IsSceneFullyLoaded())
            {
                return Result::Pending;
            }
            Snapshot(app);
            phase_ = Phase::SwitchAway;
            wait_until_frame_ = frame_ + SettleFrames;
            return Result::Pending;

        case Phase::SwitchAway: {
            if (frame_ < wait_until_frame_)
            {
                return Result::Pending;
            }
            const std::string target = rhi->SupportsHardwareRayTracing() ? "gpu" : "deferred";
            EnforceConfig("pipeline", target);
            Log(Info, "{}: switched pipeline forward -> {} at runtime", GetName(), target);
            phase_ = Phase::SwitchBack;
            wait_until_frame_ = frame_ + SettleFrames;
            return Result::Pending;
        }

        case Phase::SwitchBack:
            if (frame_ < wait_until_frame_)
            {
                return Result::Pending;
            }
            VerifyReuse(app, "switching away");
            phase_ = Phase::SnapshotBack;
            return Result::Pending;

        case Phase::SnapshotBack:
            Snapshot(app);
            phase_ = Phase::SwitchForward;
            return Result::Pending;

        case Phase::SwitchForward:
            EnforceConfig("pipeline", std::string("forward"));
            Log(Info, "{}: switched pipeline back to forward at runtime", GetName());
            phase_ = Phase::Verify;
            wait_until_frame_ = frame_ + SettleFrames;
            return Result::Pending;

        case Phase::Verify:
            if (frame_ < wait_until_frame_)
            {
                return Result::Pending;
            }
            VerifyReuse(app, "returning to forward");
            phase_ = Phase::Done;
            return Result::Pending;

        case Phase::Done:
            return failed_.load(std::memory_order_acquire) ? Result::Fail : Result::Pass;

        default:
            return Result::Fail;
        }
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 2000;
    }

private:
    enum class Phase : uint8_t
    {
        WaitLoaded,
        SwitchAway,
        SwitchBack,
        SnapshotBack,
        SwitchForward,
        Verify,
        Done,
    };

    void Snapshot(AppFramework &app)
    {
        const auto *framework = app.GetRenderFramework();
        RunOnRenderThread([this, framework] { baseline_ = framework->GetGraphTexturePool().GetStats(); });
    }

    // the settled renderer's graphs hold every image in the pool: images only the previous renderer used were released
    // after RGTexturePool::UnusedGraphsBeforeRelease graphs
    void VerifyReuse(AppFramework &app, const char *what)
    {
        const auto *framework = app.GetRenderFramework();
        RunOnRenderThread([this, framework, what] {
            const auto &pool = framework->GetGraphTexturePool();
            const auto &stats = pool.GetStats();
            const auto created = stats.num_created - baseline_.num_created;
            Log(Info, "{}: {}: graph images created {}, reused {}, released {}, held {}", GetName(), what, created,
                stats.num_reused - baseline_.num_reused, stats.num_released - baseline_.num_released,
                pool.GetImageCount());
            Expect(created < pool.GetImageCount(),
                   std::string(what) + " reuses a graph image the previous renderer left in the pool");
        });
    }

    template <typename Task> void RunOnRenderThread(Task &&task)
    {
        task_pending_.store(true, std::memory_order_release);
        TaskManager::RunInRenderThread([this, render_task = std::forward<Task>(task)] {
            render_task();
            task_pending_.store(false, std::memory_order_release);
        });
    }

    void Expect(bool condition, const std::string &what)
    {
        if (condition)
        {
            Log(Info, "{}: OK - {}", GetName(), what);
        }
        else
        {
            Log(Error, "{}: FAILED - {}", GetName(), what);
            failed_.store(true, std::memory_order_release);
        }
    }

    Phase phase_ = Phase::WaitLoaded;
    uint32_t wait_until_frame_ = 0;

    std::atomic<bool> task_pending_{false};
    std::atomic<bool> failed_{false};

    // only accessed from the render thread
    RGTexturePool::Stats baseline_;
};

static TestCaseRegistrar<PipelineSwitchPoolTest> pipeline_switch_pool_test_registrar("pipeline_switch_pool");
} // namespace sparkle
