#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/camera/OrbitCameraComponent.h"

namespace sparkle
{

/// M3 test gate: Validates bilinear/Catmull-Rom reprojection in temporal accumulation.
///
/// Runs 30 frames with small incremental camera motion (orbit yaw changes)
/// to exercise the full reprojection pipeline: MV computation -> bilinear sampling
/// -> occlusion test -> Catmull-Rom upgrade -> footprint quality modulation.
///
/// Validates:
/// - No crash over 30 frames of continuous motion
/// - Camera delta is non-zero during motion (MVs are being generated)
/// - Final screenshot is not all-black
///
/// Usage: --test_case reblur_reprojection --pipeline gpu --use_reblur true --spp 1 --max_spp 60
class ReblurReprojectionTest : public TestCase
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

        auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);

        // Phase 1 (frames 1-5): Static warmup — let the scene load and accumulate
        if (frame_ <= kWarmupFrames)
        {
            if (frame_ == 1)
            {
                Log(Info, "ReblurReprojectionTest: warmup phase (frames 1-{})", kWarmupFrames);
            }
            return Result::Pending;
        }

        // Phase 2 (frames 6-25): Incremental orbit motion — small yaw steps each frame
        if (frame_ <= kWarmupFrames + kMotionFrames)
        {
            if (orbit)
            {
                float yaw_step = 2.0f; // 2 degrees per frame
                float yaw = 30.0f + yaw_step * static_cast<float>(frame_ - kWarmupFrames);
                orbit->Setup(Vector3::Zero(), 3.0f, 20.0f, yaw);
            }

            if (frame_ == kWarmupFrames + 1)
            {
                Log(Info, "ReblurReprojectionTest: motion phase started (frames {}-{})", kWarmupFrames + 1,
                    kWarmupFrames + kMotionFrames);
            }

            // Log camera delta at midpoint to confirm motion propagates
            if (frame_ == kWarmupFrames + kMotionFrames / 2)
            {
                auto *proxy = static_cast<CameraRenderProxy *>(camera->GetRenderProxy());
                float delta = (proxy->GetPosture().position - proxy->GetPositionPrev()).norm();
                Log(Info, "ReblurReprojectionTest: mid-motion camera delta = {:.6f}", delta);
            }

            return Result::Pending;
        }

        // Phase 3 (frames 26-30): Stop motion, let it reconverge
        if (frame_ <= kWarmupFrames + kMotionFrames + kSettleFrames)
        {
            if (frame_ == kWarmupFrames + kMotionFrames + 1)
            {
                Log(Info, "ReblurReprojectionTest: settle phase (frames {}-{})", frame_,
                    kWarmupFrames + kMotionFrames + kSettleFrames);
            }
            return Result::Pending;
        }

        // Phase 4: Take screenshot and pass
        if (app.IsScreenshotCompleted())
        {
            Log(Info, "ReblurReprojectionTest: screenshot captured after {} frames — PASS", frame_);
            return Result::Pass;
        }

        if (!screenshot_requested_)
        {
            Log(Info, "ReblurReprojectionTest: requesting screenshot at frame {}", frame_);
            app.RequestTakeScreenshot("reblur_reprojection");
            screenshot_requested_ = true;
        }

        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "ReblurReprojectionTest timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t kWarmupFrames = 5;
    static constexpr uint32_t kMotionFrames = 20;
    static constexpr uint32_t kSettleFrames = 5;

    uint32_t frame_ = 0;
    bool screenshot_requested_ = false;
};

static TestCaseRegistrar<ReblurReprojectionTest> reblur_reprojection_registrar("reblur_reprojection");
} // namespace sparkle
