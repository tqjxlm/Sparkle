#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "application/TestCase.h"

namespace sparkle
{
// drags the camera a little and back to its starting point through the mouse input path.
// the view must end up exactly where it started. tests/camera/camera_nudge_test.py drives
// this test and compares the before/after screenshots.
class CameraNudgeReturnTest : public TestCase
{
public:
    // give the returned camera a few frames so the render proxy consumes the final transform
    static constexpr uint32_t SettleFrames = 10;

    void OnEnforceConfigs() override
    {
        EnforceConfig("pipeline", std::string("forward"));
    }

    Result OnTick(AppFramework &app) override
    {
        auto *render_framework = app.GetRenderFramework();

        switch (stage_)
        {
        case Stage::WaitInitialRender:
            if (render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("camera_nudge_before");
                stage_ = Stage::WaitBeforeScreenshot;
            }
            break;

        case Stage::WaitBeforeScreenshot:
            if (screenshot_->IsCompleted())
            {
                NudgeAndReturn(app);
                settle_frame_ = frame_ + SettleFrames;
                stage_ = Stage::WaitReturnRender;
            }
            break;

        case Stage::WaitReturnRender:
            if (frame_ >= settle_frame_ && render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("camera_nudge_after");
                stage_ = Stage::WaitAfterScreenshot;
            }
            break;

        case Stage::WaitAfterScreenshot:
            if (screenshot_->IsCompleted())
            {
                return Result::Pass;
            }
            break;
        }

        return Result::Pending;
    }

private:
    static void NudgeAndReturn(AppFramework &app)
    {
        constexpr float StartX = 640.f;
        constexpr float StartY = 360.f;
        constexpr float NudgePixels = 20.f;

        app.CursorPositionCallback(StartX, StartY);
        app.MouseButtonCallback(AppFramework::ClickButton::PrimaryLeft, AppFramework::KeyAction::Press, 0);
        app.CursorPositionCallback(StartX + NudgePixels, StartY + NudgePixels);
        app.CursorPositionCallback(StartX, StartY);
        app.MouseButtonCallback(AppFramework::ClickButton::PrimaryLeft, AppFramework::KeyAction::Release, 0);
    }

    enum class Stage : uint8_t
    {
        WaitInitialRender,
        WaitBeforeScreenshot,
        WaitReturnRender,
        WaitAfterScreenshot,
    };

    Stage stage_ = Stage::WaitInitialRender;

    std::shared_ptr<ScreenshotRequest> screenshot_;

    uint32_t settle_frame_ = 0;
};

static TestCaseRegistrar<CameraNudgeReturnTest> camera_nudge_return_test_registrar("camera_nudge_return");
} // namespace sparkle
