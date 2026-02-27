#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/camera/OrbitCameraComponent.h"

namespace sparkle
{

/// M2 test gate: Validates motion vector computation under camera motion.
///
/// Phase 1 (frames 0-4): Static camera warmup — screenshot should show ~zero MVs.
/// Phase 2 (frame 5):    Move camera by orbiting.
/// Phase 3 (frames 6-8): Run with motion — screenshot should show non-zero MVs.
///
/// The C++ test validates no crash. Python companion validates MV pixel values.
///
/// Usage: --test_case reblur_mv_test --pipeline gpu --use_reblur true --spp 1 --max_spp 10
class ReblurMotionVectorTest : public TestCase
{
public:
    Result Tick(AppFramework &app) override
    {
        frame_++;

        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
        }

        // Phase 1: Take static screenshot at frame 3
        if (frame_ == 3 && !static_screenshot_done_)
        {
            Log(Info, "ReblurMotionVectorTest: requesting static-camera screenshot (frame {})", frame_);
            app.RequestTakeScreenshot("reblur_mv_static");
            static_screenshot_requested_ = true;
        }

        if (static_screenshot_requested_ && app.IsScreenshotCompleted())
        {
            static_screenshot_done_ = true;
            static_screenshot_requested_ = false;
            Log(Info, "ReblurMotionVectorTest: static screenshot captured");
        }

        // Phase 2: Move camera at frame 5
        if (frame_ == 5)
        {
            auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);
            if (orbit)
            {
                orbit->Setup(Vector3::Zero(), 3.0f, 25.0f, 90.0f);
                Log(Info, "ReblurMotionVectorTest: moved orbit camera for MV generation");
            }
            else
            {
                camera->GetNode()->SetTransform(Vector3(2.0f, 0.0f, 0.0f));
                Log(Info, "ReblurMotionVectorTest: moved camera via SetTransform");
            }
        }

        // Phase 3: Take motion screenshot at frame 8
        if (frame_ == 8 && static_screenshot_done_ && !motion_screenshot_requested_)
        {
            // Verify MV should be non-zero by checking camera delta
            auto *proxy = static_cast<CameraRenderProxy *>(camera->GetRenderProxy());
            float pos_delta = (proxy->GetPosture().position - proxy->GetPositionPrev()).norm();
            Log(Info, "ReblurMotionVectorTest: camera position delta = {:.6f}", pos_delta);

            Log(Info, "ReblurMotionVectorTest: requesting motion-camera screenshot (frame {})", frame_);
            app.RequestTakeScreenshot("reblur_mv_motion");
            motion_screenshot_requested_ = true;
        }

        if (motion_screenshot_requested_ && app.IsScreenshotCompleted())
        {
            Log(Info, "ReblurMotionVectorTest: motion screenshot captured — PASS (no crash)");
            return Result::Pass;
        }

        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "ReblurMotionVectorTest timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    uint32_t frame_ = 0;
    bool static_screenshot_requested_ = false;
    bool static_screenshot_done_ = false;
    bool motion_screenshot_requested_ = false;
};

static TestCaseRegistrar<ReblurMotionVectorTest> reblur_mv_test_registrar("reblur_mv_test");
} // namespace sparkle
