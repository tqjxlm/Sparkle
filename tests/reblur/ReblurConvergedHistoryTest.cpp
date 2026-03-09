#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/camera/OrbitCameraComponent.h"

namespace sparkle
{

/// Validates that a small camera yaw delta preserves converged temporal history.
///
/// With a fully converged previous frame, a small view change should still
/// reprojection-fetch most pixels from the converged history via motion vectors.
/// The result should be nearly as clean as the converged frame, NOT a noisy
/// 1spp restart.
///
/// Phase 1: Wait for IsReadyForAutoScreenshot() — full convergence at any max_spp.
/// Phase 2: Take "before" screenshot of fully converged frame.
/// Phase 3: Apply 2° yaw delta to orbit camera.
/// Phase 4: Wait 2 frames for MV computation and reprojection.
/// Phase 5: Take "after" screenshot — should be nearly as clean.
///
/// C++ test validates no crash and logs camera delta.
/// Python companion (test_converged_history.py) validates noise levels.
///
/// Usage: --test_case reblur_converged_history
class ReblurConvergedHistoryTest : public TestCase
{
public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("pipeline", std::string("gpu"));
        EnforceConfig("use_reblur", true);
        EnforceConfig("spp", 1u);
    }

    Result OnTick(AppFramework &app) override
    {
        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
        }

        auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);

        // Phase 1: Wait for full convergence (adapts to any max_spp)
        if (!converged_)
        {
            if (frame_ == 1)
            {
                Log(Info, "{}: waiting for convergence...", GetName());
            }
            if (app.GetRenderFramework()->IsReadyForAutoScreenshot())
            {
                converged_ = true;
                Log(Info, "{}: convergence reached at frame {}", GetName(), frame_);
            }
            else
            {
                return Result::Pending;
            }
        }

        // Phase 2: Request "before" screenshot
        if (!before_request_)
        {
            Log(Info, "{}: requesting 'before' screenshot at frame {}", GetName(), frame_);
            before_request_ = app.GetRenderFramework()->RequestTakeScreenshot("converged_history_before");
            return Result::Pending;
        }

        // Wait for "before" screenshot to complete
        if (!before_done_ && before_request_->IsCompleted())
        {
            before_done_ = true;
            Log(Info, "{}: 'before' screenshot captured", GetName());
        }

        if (!before_done_)
        {
            return Result::Pending;
        }

        // Phase 3: Apply small yaw delta (once)
        if (!nudge_applied_)
        {
            if (orbit)
            {
                float old_yaw = orbit->GetYaw();
                float new_yaw = old_yaw + YawDelta;
                orbit->Setup(orbit->GetCenter(), orbit->GetRadius(), orbit->GetPitch(), new_yaw);
                Log(Info, "{}: applied {:.1f} deg yaw delta ({:.1f} -> {:.1f})", GetName(), YawDelta, old_yaw, new_yaw);
            }
            else
            {
                Log(Warn, "{}: camera is not OrbitCameraComponent, cannot nudge", GetName());
                return Result::Fail;
            }
            nudge_applied_ = true;
            nudge_frame_ = frame_;
            return Result::Pending;
        }

        // Phase 4: Wait for reprojection to happen (2 frames after nudge)
        if (frame_ < nudge_frame_ + SettleFrames)
        {
            // Log camera delta on first frame after nudge to confirm motion propagated
            if (frame_ == nudge_frame_ + 1)
            {
                auto *proxy = static_cast<CameraRenderProxy *>(camera->GetRenderProxy());
                float delta = (proxy->GetPosture().position - proxy->GetPositionPrev()).norm();
                Log(Info, "{}: camera position delta = {:.6f}", GetName(), delta);
            }
            return Result::Pending;
        }

        // Phase 5: Request "after" screenshot
        if (!after_request_)
        {
            Log(Info, "{}: requesting 'after' screenshot at frame {}", GetName(), frame_);
            after_request_ = app.GetRenderFramework()->RequestTakeScreenshot("converged_history_after");
            return Result::Pending;
        }

        if (after_request_->IsCompleted())
        {
            Log(Info, "{}: 'after' screenshot captured at frame {} — PASS", GetName(), frame_);
            return Result::Pass;
        }

        return Result::Pending;
    }

private:
    static constexpr float YawDelta = 2.0f; // degrees
    static constexpr uint32_t SettleFrames = 5;

    bool converged_ = false;
    std::shared_ptr<ScreenshotRequest> before_request_;
    std::shared_ptr<ScreenshotRequest> after_request_;
    bool before_done_ = false;
    bool nudge_applied_ = false;
    uint32_t nudge_frame_ = 0;
};

static TestCaseRegistrar<ReblurConvergedHistoryTest> reblur_converged_history_registrar("reblur_converged_history");
} // namespace sparkle
