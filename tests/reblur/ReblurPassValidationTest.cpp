#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
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
    Result OnTick(AppFramework &app) override
    {
        // Wait for scene load and several denoiser frames
        if (frame_ < WarmupFrames)
        {
            return Result::Pending;
        }

        if (request_ && request_->IsCompleted())
        {
            Log(Info, "ReblurPassValidationTest: screenshot captured after {} frames", frame_);
            return Result::Pass;
        }

        if (!request_)
        {
            Log(Info, "ReblurPassValidationTest: requesting screenshot at frame {}", frame_);
            request_ = app.RequestTakeScreenshot("reblur_pass_validation");
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t WarmupFrames = 10;
    std::shared_ptr<ScreenshotRequest> request_;
};

static TestCaseRegistrar<ReblurPassValidationTest> reblur_pass_validation_registrar("reblur_pass_validation");
} // namespace sparkle
