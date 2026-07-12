#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "rhi/RHI.h"

#include <atomic>

namespace sparkle
{
// exercises RHIRenderTargetPool end to end on the render thread:
// 1. two acquires with the same attributes return distinct targets while both are held.
// 2. a dropped target is reused for a matching request once the GPU can no longer reference it.
// 3. ReleaseUnused releases every free target; the pool never releases them on its own.
class RenderTargetPoolTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        // wait for any in-flight render thread task before advancing the state machine
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        if (failed_.load(std::memory_order_acquire))
        {
            // references were dropped by the failing task itself; safe to exit
            return Result::Fail;
        }

        auto *rhi = app.GetRHI();

        switch (stage_)
        {
        case Stage::AcquireTargets:
            RunOnRenderThread([this, rhi] {
                auto &pool = rhi->GetRenderTargetPool();
                baseline_stats_ = pool.GetStats();

                first_ = pool.Acquire(MakeAttribute(), "PoolTestFirst");
                second_ = pool.Acquire(MakeAttribute(), "PoolTestSecond");

                Expect(first_ && second_, "acquired targets are valid");
                Expect(first_.get() != second_.get(), "held targets are distinct");
                Expect(pool.GetStats().num_created == baseline_stats_.num_created + 2,
                       "both requests created new targets");

                second_raw_ = second_.get();
                // free it. it may only be reused after the GPU safety delay.
                second_ = nullptr;
            });

            stage_ = Stage::ReuseFreedTarget;
            // +2: the pool only knows the target is free at its next per-frame tick
            wait_until_frame_ = frame_ + rhi->GetMaxFramesInFlight() + 2;
            return Result::Pending;

        case Stage::ReuseFreedTarget:
            if (frame_ < wait_until_frame_)
            {
                return Result::Pending;
            }

            RunOnRenderThread([this, rhi] {
                auto &pool = rhi->GetRenderTargetPool();

                auto reused = pool.Acquire(MakeAttribute(), "PoolTestReuse");

                Expect(reused.get() == second_raw_, "freed target is reused for a matching request");
                Expect(pool.GetStats().num_reused == baseline_stats_.num_reused + 1, "reuse is counted");

                // drop everything so both targets become free
                first_ = nullptr;
            });

            stage_ = Stage::ManualRelease;
            return Result::Pending;

        case Stage::ManualRelease:
            RunOnRenderThread([this, rhi] {
                auto &pool = rhi->GetRenderTargetPool();

                Expect(pool.ReleaseUnused() >= 2, "ReleaseUnused releases both free targets");
                Expect(pool.GetStats().num_released >= baseline_stats_.num_released + 2, "release is counted");
            });

            stage_ = Stage::Done;
            return Result::Pending;

        case Stage::Done:
            return failed_.load(std::memory_order_acquire) ? Result::Fail : Result::Pass;

        default:
            return Result::Fail;
        }
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 1000;
    }

private:
    enum class Stage : uint8_t
    {
        AcquireTargets,
        ReuseFreedTarget,
        ManualRelease,
        Done,
    };

    static RHIRenderTarget::Attribute MakeAttribute()
    {
        RHIImage::Attribute image_attribute;
        image_attribute.format = PixelFormat::R8G8B8A8Unorm;
        image_attribute.width = 64;
        image_attribute.height = 64;
        image_attribute.usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::ColorAttachment;
        image_attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                                   .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                                   .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                                   .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest};

        RHIRenderTarget::Attribute attribute;
        attribute.SetColorAttribute(image_attribute, 0);
        return attribute;
    }

    template <typename Task> void RunOnRenderThread(Task &&task)
    {
        task_pending_.store(true, std::memory_order_release);
        TaskManager::RunInRenderThread([this, render_task = std::forward<Task>(task)] {
            render_task();
            if (failed_.load(std::memory_order_acquire))
            {
                // drop held references so shutdown does not report them as leaks
                first_ = nullptr;
                second_ = nullptr;
            }
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

    Stage stage_ = Stage::AcquireTargets;
    uint64_t wait_until_frame_ = 0;

    std::atomic<bool> task_pending_{false};
    std::atomic<bool> failed_{false};

    // only accessed from the render thread
    RHIRenderTargetPool::Stats baseline_stats_;
    RHIResourceRef<RHIRenderTarget> first_;
    RHIResourceRef<RHIRenderTarget> second_;
    const RHIRenderTarget *second_raw_ = nullptr;
};

static TestCaseRegistrar<RenderTargetPoolTest> render_target_pool_test_registrar("render_target_pool");
} // namespace sparkle
