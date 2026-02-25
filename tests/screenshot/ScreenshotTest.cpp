#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"

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
            app.RequestTakeScreenshot();
            requested_ = true;
        }

        ++frame_;
        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "ScreenshotTest timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    bool requested_ = false;
    uint32_t frame_ = 0;
};

static TestCaseRegistrar<ScreenshotTest> screenshot_test_registrar("screenshot");
} // namespace sparkle
