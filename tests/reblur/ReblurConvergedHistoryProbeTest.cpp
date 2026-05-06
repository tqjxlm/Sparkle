#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"
#include "scene/component/camera/OrbitCameraComponent.h"

namespace sparkle
{

/// Captures both the first post-nudge frame and the settled post-nudge frame.
///
/// This is a diagnostic companion to `reblur_converged_history` for cases where
/// the visible artifact depends on history being reset at the nudge and then
/// partially regrowing before the usual "after" screenshot is taken.
///
/// Usage: --test_case reblur_converged_history_probe
class ReblurConvergedHistoryProbeTest : public TestCase
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
        if (!orbit)
        {
            Log(Warn, "{}: camera is not OrbitCameraComponent, cannot probe", GetName());
            return Result::Fail;
        }

        if (!converged_)
        {
            if (frame_ == 1)
            {
                Log(Info, "{}: waiting for convergence...", GetName());
            }
            if (!app.GetRenderFramework()->IsReadyForAutoScreenshot())
            {
                return Result::Pending;
            }

            converged_ = true;
            Log(Info, "{}: convergence reached at frame {}", GetName(), frame_);
        }

        if (!before_request_)
        {
            Log(Info, "{}: requesting 'before' screenshot at frame {}", GetName(), frame_);
            before_request_ = app.GetRenderFramework()->RequestTakeScreenshot("converged_history_before");
            return Result::Pending;
        }

        if (!before_done_ && before_request_->IsCompleted())
        {
            before_done_ = true;
            Log(Info, "{}: 'before' screenshot captured", GetName());
        }

        if (!before_done_)
        {
            return Result::Pending;
        }

        if (!nudge_applied_)
        {
            float old_yaw = orbit->GetYaw();
            float new_yaw = old_yaw + YawDelta;
            orbit->Setup(orbit->GetCenter(), orbit->GetRadius(), orbit->GetPitch(), new_yaw);
            Log(Info, "{}: applied {:.1f} deg yaw delta ({:.1f} -> {:.1f})", GetName(), YawDelta, old_yaw, new_yaw);
            nudge_applied_ = true;
            nudge_frame_ = frame_;
            return Result::Pending;
        }

        if (!early_request_ && frame_ >= nudge_frame_ + EarlyFrames)
        {
            Log(Info, "{}: requesting 'after_early' screenshot at frame {}", GetName(), frame_);
            early_request_ = app.GetRenderFramework()->RequestTakeScreenshot("converged_history_after_early");
            return Result::Pending;
        }

        if (!early_done_ && early_request_ && early_request_->IsCompleted())
        {
            early_done_ = true;
            Log(Info, "{}: 'after_early' screenshot captured", GetName());
        }

        if (!final_request_ && frame_ >= nudge_frame_ + FinalFrames)
        {
            Log(Info, "{}: requesting 'after' screenshot at frame {}", GetName(), frame_);
            final_request_ = app.GetRenderFramework()->RequestTakeScreenshot("converged_history_after");
            return Result::Pending;
        }

        if (final_request_ && final_request_->IsCompleted())
        {
            Log(Info, "{}: 'after' screenshot captured at frame {} — PASS", GetName(), frame_);
            return Result::Pass;
        }

        return Result::Pending;
    }

private:
    static constexpr float YawDelta = 2.0f;
    static constexpr uint32_t EarlyFrames = 1;
    static constexpr uint32_t FinalFrames = 5;

    bool converged_ = false;
    bool before_done_ = false;
    bool nudge_applied_ = false;
    bool early_done_ = false;
    uint32_t nudge_frame_ = 0;
    std::shared_ptr<ScreenshotRequest> before_request_;
    std::shared_ptr<ScreenshotRequest> early_request_;
    std::shared_ptr<ScreenshotRequest> final_request_;
};

static TestCaseRegistrar<ReblurConvergedHistoryProbeTest> reblur_converged_history_probe_registrar(
    "reblur_converged_history_probe");
} // namespace sparkle
