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
            if (app.GetRenderConfig().clear_screenshots)
            {
                AppFramework::ClearScreenshots();
            }
            app.RequestTakeScreenshot();
            requested_ = true;
        }

        ++frame_;
        if (frame_ > MaxFrames)
        {
            Log(Error, "ScreenshotTest timed out after {} frames", MaxFrames);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t MaxFrames = 600;
    bool requested_ = false;
    uint32_t frame_ = 0;
};

static TestCaseRegistrar<ScreenshotTest> screenshot_test_registrar("screenshot");
} // namespace sparkle
