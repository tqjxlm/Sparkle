#include "application/TestCase.h"

#include "application/AppFramework.h"

namespace sparkle
{
class ScreenshotTest : public TestCase
{
public:
    Result Tick(AppFramework &app) override
    {
        if (app.IsScreenshotCompleted())
        {
            return Result::Pass;
        }

        if (!requested_)
        {
            if (app.GetRenderConfig().clear_screenshots)
            {
                AppFramework::ClearScreenshots();
            }
            app.RequestTakeScreenshot();
            requested_ = true;
        }

        return Result::Pending;
    }

private:
    bool requested_ = false;
};

static TestCaseRegistrar<ScreenshotTest> screenshot_test_registrar("screenshot");
} // namespace sparkle
