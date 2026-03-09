#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/camera/OrbitCameraComponent.h"

namespace sparkle
{

/// Vanilla GPU baseline: fully converge, nudge camera, fully re-converge.
///
/// Produces a "best case" reference pair: both screenshots are noise-free,
/// so the only difference is the geometric/lighting change from the 2-degree
/// yaw delta.  Compare against the reblur converged-history test to gauge
/// how much quality the reprojection path loses.
///
/// Phase 1: Wait for IsReadyForAutoScreenshot() — full convergence.
/// Phase 2: Take "before" screenshot.
/// Phase 3: Apply 2-degree yaw delta.
/// Phase 4: Wait for IsReadyForAutoScreenshot() to become false (reset confirmed).
/// Phase 5: Wait for IsReadyForAutoScreenshot() to become true (full re-convergence).
/// Phase 6: Take "after" screenshot.
///
/// Usage: --test_case vanilla_converged_baseline --pipeline gpu --spp 1
class VanillaConvergedBaselineTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
        }

        // Phase 1: Wait for full convergence
        if (!first_converged_)
        {
            if (app.GetRenderFramework()->IsReadyForAutoScreenshot())
            {
                first_converged_ = true;
                Log(Info, "{}: first convergence reached at frame {}", GetName(), frame_);
            }
            else
            {
                if (frame_ == 1)
                {
                    Log(Info, "{}: waiting for first convergence...", GetName());
                }
                return Result::Pending;
            }
        }

        // Phase 2: Take "before" screenshot
        if (!before_request_)
        {
            Log(Info, "{}: requesting 'before' screenshot at frame {}", GetName(), frame_);
            before_request_ = app.GetRenderFramework()->RequestTakeScreenshot("vanilla_baseline_before");
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

        // Phase 3: Apply 2-degree yaw delta (once)
        if (!nudge_applied_)
        {
            auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);
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
            return Result::Pending;
        }

        // Phase 4: Wait for sample-count reset to propagate.
        // IsReadyForAutoScreenshot() may still be true from the first convergence
        // until the render thread processes the camera change.  Wait for it to
        // become false before waiting for re-convergence.
        if (!reset_confirmed_)
        {
            if (!app.GetRenderFramework()->IsReadyForAutoScreenshot())
            {
                reset_confirmed_ = true;
                Log(Info, "{}: reset confirmed at frame {}", GetName(), frame_);
            }
            else
            {
                return Result::Pending;
            }
        }

        // Phase 5: Wait for full re-convergence
        if (!second_converged_)
        {
            if (app.GetRenderFramework()->IsReadyForAutoScreenshot())
            {
                second_converged_ = true;
                Log(Info, "{}: second convergence reached at frame {}", GetName(), frame_);
            }
            else
            {
                return Result::Pending;
            }
        }

        // Phase 6: Take "after" screenshot
        if (!after_request_)
        {
            Log(Info, "{}: requesting 'after' screenshot at frame {}", GetName(), frame_);
            after_request_ = app.GetRenderFramework()->RequestTakeScreenshot("vanilla_baseline_after");
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

    bool first_converged_ = false;
    bool before_done_ = false;
    bool nudge_applied_ = false;
    bool reset_confirmed_ = false;
    bool second_converged_ = false;

    std::shared_ptr<ScreenshotRequest> before_request_;
    std::shared_ptr<ScreenshotRequest> after_request_;
};

static TestCaseRegistrar<VanillaConvergedBaselineTest> vanilla_converged_baseline_registrar(
    "vanilla_converged_baseline");
} // namespace sparkle
