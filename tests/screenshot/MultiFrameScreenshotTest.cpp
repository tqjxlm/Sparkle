#include "application/TestCase.h"

#include "application/AppFramework.h"
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

        if (!ready_ && app.IsReadyForAutoScreenshot())
        {
            ready_ = true;
            Log(Info, "MultiFrameScreenshotTest: renderer ready at frame {}", frame_);
        }

        if (!ready_)
        {
            return Result::Pending;
        }

        if (waiting_for_completion_)
        {
            if (app.IsScreenshotCompleted())
            {
                waiting_for_completion_ = false;
                captured_count_++;
                Log(Info, "MultiFrameScreenshotTest: captured frame {} of {}", captured_count_, FramesToCapture);
            }
            return captured_count_ >= FramesToCapture ? Result::Pass : Result::Pending;
        }

        // Request next screenshot
        auto name = std::format("multi_frame_{}", captured_count_);
        app.RequestTakeScreenshot(name);
        waiting_for_completion_ = true;
        return captured_count_ >= FramesToCapture ? Result::Pass : Result::Pending;
    }

private:
    static constexpr uint32_t FramesToCapture = 5;
    uint32_t captured_count_ = 0;
    bool ready_ = false;
    bool waiting_for_completion_ = false;
};

static TestCaseRegistrar<MultiFrameScreenshotTest> multi_frame_screenshot_registrar("multi_frame_screenshot");
} // namespace sparkle
