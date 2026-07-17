#if FRAMEWORK_ANDROID

#include "application/AppFramework.h"
#include "application/NativeView.h"
#include "application/RenderFramework.h"
#include "application/TestCase.h"
#include "core/Logger.h"

namespace sparkle
{
// screenshots the loaded scene, asks the test harness to destroy and restore the native
// window (adb screen off/on), and screenshots again after the surface recovers. the
// recreated surface and swapchain must reproduce the original frame.
// tests/android/surface_loss_test.py compares the before/after screenshots.
class SurfaceLossRecoveryTest : public TestCase
{
public:
    // give the recovered swapchain a few frames so a full frame lands in it
    static constexpr uint32_t SettleFrames = 10;

    void OnEnforceConfigs() override
    {
        // the test is about the native window; it cannot run headless
        EnforceConfig("headless", false);
        EnforceConfig("pipeline", std::string("forward"));
    }

    Result OnTick(AppFramework &app) override
    {
        auto *render_framework = app.GetRenderFramework();
        auto *view = app.GetNativeView();

        switch (stage_)
        {
        case Stage::WaitInitialRender:
            if (view->CanRender() && render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("surface_loss_before");
                stage_ = Stage::WaitBeforeScreenshot;
            }
            break;

        case Stage::WaitBeforeScreenshot:
            if (screenshot_->IsCompleted())
            {
                // the harness watches for this marker and toggles the device screen
                Log(Info, "[TestCaseAction] cycle_window");
                stage_ = Stage::AwaitSurfaceLoss;
            }
            break;

        case Stage::AwaitSurfaceLoss:
            if (!view->CanRender())
            {
                Log(Info, "surface lost");
                stage_ = Stage::AwaitSurfaceRecovery;
            }
            break;

        case Stage::AwaitSurfaceRecovery:
            if (view->CanRender())
            {
                Log(Info, "surface recovered");
                settle_frame_ = frame_ + SettleFrames;
                stage_ = Stage::WaitRecoveredRender;
            }
            break;

        case Stage::WaitRecoveredRender:
            if (frame_ >= settle_frame_ && render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("surface_loss_after");
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
    enum class Stage : uint8_t
    {
        WaitInitialRender,
        WaitBeforeScreenshot,
        AwaitSurfaceLoss,
        AwaitSurfaceRecovery,
        WaitRecoveredRender,
        WaitAfterScreenshot,
    };

    Stage stage_ = Stage::WaitInitialRender;

    std::shared_ptr<ScreenshotRequest> screenshot_;

    uint32_t settle_frame_ = 0;
};

static TestCaseRegistrar<SurfaceLossRecoveryTest> surface_loss_recovery_test_registrar("surface_loss_recovery");
} // namespace sparkle

#endif
