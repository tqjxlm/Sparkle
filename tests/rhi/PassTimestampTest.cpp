#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "rhi/RHI.h"

#include <atomic>

namespace sparkle
{
// records a timed render pass twice and an untimed one once per frame until the timed pass's frame slot comes back
// with a GPU time, which may be 0 on devices that cannot resolve it. a device without pass timestamps must report no
// time at all.
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
            if (!timed_pass_)
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
        auto image = rhi->CreateImage(image_attribute, "PassTimestampTestImage");
        auto target = rhi->CreateRenderTarget({}, image, nullptr, "PassTimestampTestTarget");

        RHIRenderPass::Attribute pass_attribute;
        pass_attribute.color_load_op = RHIRenderPass::LoadOp::Clear;
        untimed_pass_ = rhi->CreateRenderPass(pass_attribute, target, "PassTimestampTestUntimedPass");

        pass_attribute.need_timestamp = true;
        timed_pass_ = rhi->CreateRenderPass(pass_attribute, target, "PassTimestampTestTimedPass");

        target_ = target;
        supported_ = rhi->SupportsPassTimestamps();
        Log(Info, "{}: pass timestamps supported: {}", GetName(), supported_);
    }

    void RecordAndCheck(RHIContext *rhi)
    {
        rhi->BeginCommandBuffer();
        auto *command_context = rhi->GetCommandContext();
        // the second run of the timed pass must not read the timer its first run just began
        for (const auto &pass : {timed_pass_, timed_pass_, untimed_pass_})
        {
            command_context->BeginRenderPass(pass);
            command_context->EndRenderPass();
        }
        rhi->SubmitCommandBuffer();

        // beginning a pass reads the time its slot measured in an earlier frame
        const auto frame_index = rhi->GetFrameIndex();
        const float timed_ms = timed_pass_->GetExecutionTime(frame_index);
        Expect(untimed_pass_->GetExecutionTime(frame_index) < 0.f, "an untimed render pass reports no time");
        if (!supported_)
        {
            Expect(timed_ms < 0.f, "a timed render pass reports no time without pass timestamps");
        }

        recordings_++;
        if (supported_ && timed_ms >= 0.f)
        {
            Log(Info, "{}: timed render pass took {:.6f} ms", GetName(), timed_ms);
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
        timed_pass_ = nullptr;
        untimed_pass_ = nullptr;
        target_ = nullptr;
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

    RHIResourceRef<RHIRenderTarget> target_;
    RHIResourceRef<RHIRenderPass> timed_pass_;
    RHIResourceRef<RHIRenderPass> untimed_pass_;
    bool supported_ = false;
    unsigned recordings_ = 0;
    std::atomic<bool> task_pending_{false};
    std::atomic<bool> done_{false};
    std::atomic<bool> failed_{false};
};

static TestCaseRegistrar<PassTimestampTest> pass_timestamp_test_registrar("pass_timestamps");
} // namespace sparkle
