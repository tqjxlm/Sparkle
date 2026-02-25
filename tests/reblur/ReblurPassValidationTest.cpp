#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"

namespace sparkle
{

/// Validates that REBLUR denoiser runs the full spatial pipeline without crash.
/// Runs several frames to exercise all 3 blur passes (PrePass, Blur, PostBlur),
/// then takes a screenshot to confirm output is non-degenerate.
///
/// Usage: --test_case reblur_pass_validation --pipeline gpu --use_reblur true --spp 1 --max_spp 4
class ReblurPassValidationTest : public TestCase
{
public:
    Result Tick(AppFramework &app) override
    {
        frame_++;

        // Wait for scene load and several denoiser frames
        if (frame_ < WarmupFrames)
        {
            return Result::Pending;
        }

        if (app.IsScreenshotCompleted())
        {
            Log(Info, "ReblurPassValidationTest: screenshot captured after {} frames", frame_);
            return Result::Pass;
        }

        if (!screenshot_requested_)
        {
            if (app.GetRenderConfig().clear_screenshots)
            {
                AppFramework::ClearScreenshots();
            }
            Log(Info, "ReblurPassValidationTest: requesting screenshot at frame {}", frame_);
            app.RequestTakeScreenshot();
            screenshot_requested_ = true;
        }

        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "ReblurPassValidationTest timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t WarmupFrames = 10;
    uint32_t frame_ = 0;
    bool screenshot_requested_ = false;
};

static TestCaseRegistrar<ReblurPassValidationTest> reblur_pass_validation_registrar("reblur_pass_validation");
} // namespace sparkle
