#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "rhi/RHI.h"

#include <atomic>

namespace sparkle
{
// records a clear timed by one pass twice, one timed by another pass once and one by an untimed pass per frame until
// the timed passes' frame slot comes back with their GPU times. the pass recorded twice reports its last run, so it
// measures 0 only on a device that also measures 0 for the pass recorded once. a device without pass timestamps must
// report no time at all.
class PassTimestampTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        if (done_.load(std::memory_order_acquire))
        {
            return failed_.load(std::memory_order_acquire) ? Result::Fail : Result::Pass;
        }

        task_pending_.store(true, std::memory_order_release);

        auto *rhi = app.GetRHI();
        TaskManager::RunInRenderThread([this, rhi] {
            if (!twice_timed_pass_)
            {
                CreatePasses(rhi);
            }

            RecordAndCheck(rhi);

            task_pending_.store(false, std::memory_order_release);
        });

        return Result::Pending;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 1000;
    }

private:
    // enough frames for every slot to come back several times
    static constexpr unsigned MaxRecordings = 32;

    void CreatePasses(RHIContext *rhi)
    {
        RHIImage::Attribute image_attribute;
        image_attribute.format = PixelFormat::R8G8B8A8Unorm;
        image_attribute.width = 4;
        image_attribute.height = 4;
        image_attribute.usages = RHIImage::ImageUsage::ColorAttachment;
        image_ = rhi->CreateImage(image_attribute, "PassTimestampTestImage");

        untimed_pass_ = rhi->CreateRenderPass("PassTimestampTestUntimedPass", false);
        twice_timed_pass_ = rhi->CreateRenderPass("PassTimestampTestTwiceTimedPass", true);
        once_timed_pass_ = rhi->CreateRenderPass("PassTimestampTestOnceTimedPass", true);

        supported_ = rhi->SupportsPassTimestamps();
        Log(Info, "{}: pass timestamps supported: {}", GetName(), supported_);
    }

    void RecordAndCheck(RHIContext *rhi)
    {
        RHIRenderingInfo info;
        info.color_attachments[0] = {.image = image_.get(), .load_op = RHILoadOp::Clear};
        info.width = image_->GetWidth();
        info.height = image_->GetHeight();

        rhi->BeginCommandBuffer();
        auto *command_context = rhi->GetCommandContext();
        // the second run of the timed pass must not read the timer its first run just began
        for (const auto &pass : {twice_timed_pass_, twice_timed_pass_, once_timed_pass_, untimed_pass_})
        {
            const auto barriers = image_->TrackTransition({.target_layout = RHIImageLayout::ColorOutput,
                                                           .after_stage = RHIPipelineStage::ColorOutput,
                                                           .before_stage = RHIPipelineStage::Bottom});
            command_context->BeginRendering(info, pass->GetName(), pass.get(), barriers);
            command_context->EndRendering();
        }
        rhi->SubmitCommandBuffer();

        // beginning a pass reads the time its slot measured in an earlier frame
        const auto frame_index = rhi->GetFrameIndex();
        const float twice_ms = twice_timed_pass_->GetExecutionTime(frame_index);
        const float once_ms = once_timed_pass_->GetExecutionTime(frame_index);
        Expect(untimed_pass_->GetExecutionTime(frame_index) < 0.f, "an untimed render pass reports no time");
        if (!supported_)
        {
            Expect(twice_ms < 0.f && once_ms < 0.f, "a timed render pass reports no time without pass timestamps");
        }

        recordings_++;
        if (supported_ && twice_ms >= 0.f && once_ms >= 0.f)
        {
            Log(Info, "{}: timed render pass took {:.6f} ms recorded twice, {:.6f} ms recorded once", GetName(),
                twice_ms, once_ms);
            Expect(twice_ms > 0.f || once_ms == 0.f, "a render pass recorded twice reports the time of its last run");
            Finish();
        }
        else if (recordings_ == MaxRecordings)
        {
            Expect(!supported_, "a timed render pass reports its GPU time");
            Finish();
        }
    }

    void Finish()
    {
        twice_timed_pass_ = nullptr;
        once_timed_pass_ = nullptr;
        untimed_pass_ = nullptr;
        image_ = nullptr;
        done_.store(true, std::memory_order_release);
    }

    void Expect(bool condition, const std::string &what)
    {
        if (condition)
        {
            return;
        }

        Log(Error, "{}: FAILED - {}", GetName(), what);
        failed_.store(true, std::memory_order_release);
    }

    RHIResourceRef<RHIImage> image_;
    RHIResourceRef<RHIPass> twice_timed_pass_;
    RHIResourceRef<RHIPass> once_timed_pass_;
    RHIResourceRef<RHIPass> untimed_pass_;
    bool supported_ = false;
    unsigned recordings_ = 0;
    std::atomic<bool> task_pending_{false};
    std::atomic<bool> done_{false};
    std::atomic<bool> failed_{false};
};

static TestCaseRegistrar<PassTimestampTest> pass_timestamp_test_registrar("pass_timestamps");
} // namespace sparkle
