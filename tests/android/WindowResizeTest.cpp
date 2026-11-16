#if FRAMEWORK_ANDROID

#include "application/AppFramework.h"
#include "application/NativeView.h"
#include "application/RenderFramework.h"
#include "application/TestCase.h"
#include "core/Logger.h"

#include <cmath>

namespace sparkle
{
// asks the test harness to resize the display (adb wm size) once the scene renders. the native window follows the
// display to a new aspect ratio, and the output resolution must follow the window.
class WindowResizeTest : public TestCase
{
public:
    // give the resized swapchain a few frames
    static constexpr uint32_t SettleFrames = 10;

    void OnEnforceConfigs() override
    {
        // the test is about the native window; it cannot run headless
        EnforceConfig("headless", false);
        EnforceConfig("pipeline", std::string("forward"));
    }

    Result OnTick(AppFramework &app) override
    {
        auto *view = app.GetNativeView();
        if (!view->CanRender())
        {
            return Result::Pending;
        }

        int width;
        int height;
        view->GetFrameBufferSize(width, height);

        switch (stage_)
        {
        case Stage::WaitInitialRender:
            if (app.GetRenderFramework()->IsReadyForAutoScreenshot())
            {
                initial_width_ = width;
                initial_height_ = height;

                // the harness watches for this marker and resizes the display
                Log(Info, "[TestCaseAction] resize_display");
                stage_ = Stage::AwaitResize;
            }
            break;

        case Stage::AwaitResize:
            if (width != initial_width_ || height != initial_height_)
            {
                settle_frame_ = frame_ + SettleFrames;
                stage_ = Stage::WaitResizedRender;
            }
            break;

        case Stage::WaitResizedRender:
            if (frame_ >= settle_frame_)
            {
                const auto &config = app.GetRenderConfig();
                Log(Info, "frame buffer [{}, {}] -> [{}, {}], output [{}, {}]", initial_width_, initial_height_, width,
                    height, config.image_width, config.image_height);

                const float aspect = AspectRatio(width, height);
                Expect(std::abs(aspect - AspectRatio(initial_width_, initial_height_)) > AspectTolerance,
                       "the resized window has a new aspect ratio");
                Expect(std::abs(aspect - AspectRatio(config.image_width, config.image_height)) < AspectTolerance,
                       "the output aspect ratio follows the frame buffer");

                return HasFailed() ? Result::Fail : Result::Pass;
            }
            break;
        }

        return Result::Pending;
    }

private:
    static constexpr float AspectTolerance = 0.01f;

    enum class Stage : uint8_t
    {
        WaitInitialRender,
        AwaitResize,
        WaitResizedRender,
    };

    static float AspectRatio(auto width, auto height)
    {
        return static_cast<float>(width) / static_cast<float>(height);
    }

    Stage stage_ = Stage::WaitInitialRender;

    int initial_width_ = 0;
    int initial_height_ = 0;

    uint32_t settle_frame_ = 0;
};

static TestCaseRegistrar<WindowResizeTest> window_resize_test_registrar("window_resize");
} // namespace sparkle

#endif
