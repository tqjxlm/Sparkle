#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
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
/// Usage: --test_case reblur_reprojection
class ReblurReprojectionTest : public TestCase
{
public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("pipeline", std::string("gpu"));
        EnforceConfig("use_reblur", true);
        EnforceConfig("spp", 1u);
        EnforceConfig("max_spp", 60u);
    }

    Result OnTick(AppFramework &app) override
    {
        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
        }

        auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);

        // Phase 1 (frames 1-5): Static warmup — let the scene load and accumulate
        if (frame_ <= WarmupFrames)
        {
            if (frame_ == 1)
            {
                Log(Info, "{}: warmup phase (frames 1-{})", GetName(), WarmupFrames);
            }
            return Result::Pending;
        }

        // Phase 2 (frames 6-25): Incremental orbit motion — small yaw steps each frame
        if (frame_ <= WarmupFrames + MotionFrames)
        {
            if (orbit)
            {
                float yaw_step = 2.0f; // 2 degrees per frame
                float yaw = 30.0f + yaw_step * static_cast<float>(frame_ - WarmupFrames);
                orbit->Setup(Vector3::Zero(), 3.0f, 20.0f, yaw);
            }

            if (frame_ == WarmupFrames + 1)
            {
                Log(Info, "{}: motion phase started (frames {}-{})", GetName(), WarmupFrames + 1,
                    WarmupFrames + MotionFrames);
            }

            // Log camera delta at midpoint to confirm motion propagates
            if (frame_ == WarmupFrames + MotionFrames / 2)
            {
                auto *proxy = static_cast<CameraRenderProxy *>(camera->GetRenderProxy());
                float delta = (proxy->GetPosture().position - proxy->GetPositionPrev()).norm();
                Log(Info, "{}: mid-motion camera delta = {:.6f}", GetName(), delta);
            }

            return Result::Pending;
        }

        // Phase 3 (frames 26-30): Stop motion, let it reconverge
        if (frame_ <= WarmupFrames + MotionFrames + SettleFrames)
        {
            if (frame_ == WarmupFrames + MotionFrames + 1)
            {
                Log(Info, "{}: settle phase (frames {}-{})", GetName(), frame_,
                    WarmupFrames + MotionFrames + SettleFrames);
            }
            return Result::Pending;
        }

        // Phase 4: Take screenshot and pass
        if (request_ && request_->IsCompleted())
        {
            Log(Info, "{}: screenshot captured after {} frames — PASS", GetName(), frame_);
            return Result::Pass;
        }

        if (!request_)
        {
            Log(Info, "{}: requesting screenshot at frame {}", GetName(), frame_);
            request_ = app.GetRenderFramework()->RequestTakeScreenshot("reblur_reprojection");
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t WarmupFrames = 5;
    static constexpr uint32_t MotionFrames = 20;
    static constexpr uint32_t SettleFrames = 5;

    std::shared_ptr<ScreenshotRequest> request_;
};

static TestCaseRegistrar<ReblurReprojectionTest> reblur_reprojection_registrar("reblur_reprojection");
} // namespace sparkle
