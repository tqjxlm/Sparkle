#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "rhi/RHI.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sparkle
{
// runs windowed. once the scene renders, flushes the render thread twice in each of several frames: each flush hands
// the render thread a batch of its own, which it renders a frame for, while windowed metal acquires one drawable per
// main thread frame, so most of those frames find no drawable. the render thread must skip them and go on presenting
// the frames of the main thread.
class WindowedFlushTest : public TestCase
{
    static constexpr uint32_t FlushFrames = 10;
    static constexpr uint32_t FlushesPerFrame = 2;
    static constexpr uint32_t SettleFrames = 30;

public:
    void OnEnforceConfigs() override
    {
        // the subject is the window's drawables: headless metal throttles its frames on the render thread instead
        EnforceConfig("headless", false);
        EnforceConfig("pipeline", std::string("forward"));
    }

    Result OnTick(AppFramework &app) override
    {
        switch (phase_)
        {
        case Phase::WaitRendering:
            if (app.GetRenderFramework()->IsReadyForAutoScreenshot())
            {
                phase_ = Phase::Flush;
                phase_end_frame_ = frame_ + FlushFrames;
            }
            return Result::Pending;

        case Phase::Flush:
            for (uint32_t i = 0; i < FlushesPerFrame; i++)
            {
                TaskManager::FlushRenderThread();
            }
            if (frame_ + 1 >= phase_end_frame_)
            {
                settle_start_ = CountRenderedFrames(app);
                phase_ = Phase::Settle;
                phase_end_frame_ = frame_ + SettleFrames;
            }
            return Result::Pending;

        case Phase::Settle:
            if (frame_ >= phase_end_frame_)
            {
                settle_end_ = CountRenderedFrames(app);
                phase_ = Phase::Verify;
            }
            return Result::Pending;

        case Phase::Verify: {
            if (!settle_end_->IsReady())
            {
                return Result::Pending;
            }
            const auto rendered = settle_end_->Get() - settle_start_->Get();
            Log(Info, "{}: the render thread rendered {} frames in {} main thread frames after the flushes", GetName(),
                rendered, SettleFrames);
            Expect(rendered >= SettleFrames / 2, "the render thread presents the main thread frames after the flushes");
            return HasFailed() ? Result::Fail : Result::Pass;
        }

        default:
            return Result::Fail;
        }
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 600;
    }

private:
    enum class Phase : uint8_t
    {
        WaitRendering,
        Flush,
        Settle,
        Verify,
    };

    // the frames the render thread rendered by the time it runs this frame's batch
    static std::shared_ptr<TaskFuture<uint64_t>> CountRenderedFrames(AppFramework &app)
    {
        auto *rhi = app.GetRHI();
        return TaskManager::RunInRenderThread([rhi] { return rhi->GetRenderedFrameCount(); });
    }

    Phase phase_ = Phase::WaitRendering;
    uint32_t phase_end_frame_ = 0;

    std::shared_ptr<TaskFuture<uint64_t>> settle_start_;
    std::shared_ptr<TaskFuture<uint64_t>> settle_end_;
};

static TestCaseRegistrar<WindowedFlushTest> windowed_flush_test_registrar("windowed_flush");
} // namespace sparkle
