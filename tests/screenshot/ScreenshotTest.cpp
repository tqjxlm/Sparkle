#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"

namespace sparkle
{
class ScreenshotTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (request_ && request_->IsCompleted())
        {
            return Result::Pass;
        }

        if (!request_ && app.GetRenderFramework()->IsReadyForAutoScreenshot())
        {
            request_ = app.GetRenderFramework()->RequestTakeScreenshot("screenshot");
        }

        return Result::Pending;
    }

private:
    std::shared_ptr<ScreenshotRequest> request_;
};

static TestCaseRegistrar<ScreenshotTest> screenshot_test_registrar("screenshot");
} // namespace sparkle
