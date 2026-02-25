#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"

namespace sparkle
{
class ReblurSmokeTest : public TestCase
{
public:
    Result Tick(AppFramework &app) override
    {
        frame_count_++;

        // Wait for convergence (30 frames with REBLUR)
        if (frame_count_ < 30)
            return Result::Pending;

        if (app.IsScreenshotCompleted())
            return Result::Pass;

        if (!requested_)
        {
            app.RequestTakeScreenshot();
            requested_ = true;
        }

        // Safety timeout (honours test_timeout cvar from CI)
        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_count_ > timeout)
        {
            Log(Error, "ReblurSmokeTest timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    uint32_t frame_count_ = 0;
    bool requested_ = false;
};

static TestCaseRegistrar<ReblurSmokeTest> reblur_smoke_registrar("reblur_smoke");
} // namespace sparkle
