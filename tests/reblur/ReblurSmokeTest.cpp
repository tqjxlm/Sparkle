#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"

namespace sparkle
{
class ReblurSmokeTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        // Wait for convergence (30 frames with REBLUR)
        if (frame_ < 30)
            return Result::Pending;

        if (request_ && request_->IsCompleted())
            return Result::Pass;

        if (!request_)
        {
            request_ = app.GetRenderFramework()->RequestTakeScreenshot("reblur_smoke");
        }

        return Result::Pending;
    }

private:
    std::shared_ptr<ScreenshotRequest> request_;
};

static TestCaseRegistrar<ReblurSmokeTest> reblur_smoke_registrar("reblur_smoke");
} // namespace sparkle
