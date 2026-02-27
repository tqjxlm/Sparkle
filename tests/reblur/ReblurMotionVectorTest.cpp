#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
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
    Result OnTick(AppFramework &app) override
    {
        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
        }

        // Phase 1: Take static screenshot at frame 3
        if (frame_ == 3 && !static_request_)
        {
            Log(Info, "ReblurMotionVectorTest: requesting static-camera screenshot (frame {})", frame_);
            static_request_ = app.RequestTakeScreenshot("reblur_mv_static");
        }

        if (static_request_ && !static_screenshot_done_ && static_request_->IsCompleted())
        {
            static_screenshot_done_ = true;
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
        if (frame_ == 8 && static_screenshot_done_ && !motion_request_)
        {
            // Verify MV should be non-zero by checking camera delta
            auto *proxy = static_cast<CameraRenderProxy *>(camera->GetRenderProxy());
            float pos_delta = (proxy->GetPosture().position - proxy->GetPositionPrev()).norm();
            Log(Info, "ReblurMotionVectorTest: camera position delta = {:.6f}", pos_delta);

            Log(Info, "ReblurMotionVectorTest: requesting motion-camera screenshot (frame {})", frame_);
            motion_request_ = app.RequestTakeScreenshot("reblur_mv_motion");
        }

        if (motion_request_ && motion_request_->IsCompleted())
        {
            Log(Info, "ReblurMotionVectorTest: motion screenshot captured — PASS (no crash)");
            return Result::Pass;
        }

        return Result::Pending;
    }

private:
    std::shared_ptr<ScreenshotRequest> static_request_;
    std::shared_ptr<ScreenshotRequest> motion_request_;
    bool static_screenshot_done_ = false;
};

static TestCaseRegistrar<ReblurMotionVectorTest> reblur_mv_test_registrar("reblur_mv_test");
} // namespace sparkle
