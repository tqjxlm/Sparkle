#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"

#include <format>

namespace sparkle
{

/// Captures multiple screenshots after the renderer reaches max_spp.
/// Useful for verifying temporal stability of denoiser output across frames.
///
/// Usage: --test_case multi_frame_screenshot --pipeline gpu --max_spp 16 --test_timeout 500
class MultiFrameScreenshotTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (captured_count_ >= FramesToCapture)
        {
            return Result::Pass;
        }

        if (!ready_to_capture_ && app.IsReadyForAutoScreenshot())
        {
            ready_to_capture_ = true;
            Log(Info, "MultiFrameScreenshotTest: renderer ready at frame {}", frame_);
        }

        if (!ready_to_capture_)
        {
            return Result::Pending;
        }

        if (active_request_)
        {
            if (active_request_->IsCompleted())
            {
                active_request_.reset();
                captured_count_++;
                Log(Info, "MultiFrameScreenshotTest: captured frame {} of {}", captured_count_, FramesToCapture);
            }
            return captured_count_ >= FramesToCapture ? Result::Pass : Result::Pending;
        }

        // Request next screenshot
        auto name = std::format("multi_frame_{}", captured_count_);
        active_request_ = app.RequestTakeScreenshot(name);
        return Result::Pending;
    }

private:
    static constexpr uint32_t FramesToCapture = 5;
    uint32_t captured_count_ = 0;
    bool ready_to_capture_ = false;
    std::shared_ptr<ScreenshotRequest> active_request_;
};

static TestCaseRegistrar<MultiFrameScreenshotTest> multi_frame_screenshot_registrar("multi_frame_screenshot");
} // namespace sparkle
