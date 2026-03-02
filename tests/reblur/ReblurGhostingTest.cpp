#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"
#include "scene/component/camera/OrbitCameraComponent.h"

#include <format>

namespace sparkle
{

/// Ghosting detection test: nudges camera and captures screenshots to reveal
/// object-on-floor ghosting artifacts from cross-material history blending.
///
/// Phases:
///   1. Static warmup: accumulate history for WarmupFrames
///   2. Nudge loop: for each nudge, rotate yaw by YawStep degrees, accumulate
///      for AccumFrames, then capture a screenshot. Repeat NudgeCount times.
///   3. Final settle: hold static for SettleFrames, capture final screenshot.
///
/// Screenshots are named "ghosting_before" (pre-motion baseline),
/// "ghosting_nudge_N" (after each nudge), and "ghosting_settled" (reconverged).
///
/// Usage: --test_case reblur_ghosting --pipeline gpu --use_reblur true --spp 1 --max_spp 200
class ReblurGhostingTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
        }

        auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);

        // Wait for pending screenshot
        if (active_request_)
        {
            if (active_request_->IsCompleted())
            {
                Log(Info, "ReblurGhostingTest: captured '{}' at frame {}", active_request_->GetName(), frame_);
                active_request_.reset();
            }
            else
            {
                return Result::Pending;
            }
        }

        // Phase 1: Static warmup
        if (frame_ <= WarmupFrames)
        {
            if (frame_ == 1 && orbit)
            {
                // Record initial pose
                initial_yaw_ = orbit->GetYaw();
                current_yaw_ = initial_yaw_;
                Log(Info, "ReblurGhostingTest: initial yaw = {:.1f}", initial_yaw_);
            }
            return Result::Pending;
        }

        // Capture "before" baseline screenshot (converged, pre-motion)
        if (!before_captured_)
        {
            active_request_ = app.RequestTakeScreenshot("ghosting_before");
            before_captured_ = true;
            Log(Info, "ReblurGhostingTest: capturing 'before' at frame {}", frame_);
            return Result::Pending;
        }

        // Phase 2: Nudge loop
        if (nudge_index_ < NudgeCount)
        {
            // Apply camera nudge at start of each nudge cycle
            if (!nudge_applied_)
            {
                current_yaw_ += YawStep;
                if (orbit)
                {
                    orbit->Setup(orbit->GetCenter(), orbit->GetRadius(), orbit->GetPitch(), current_yaw_);
                }
                nudge_frame_ = frame_;
                nudge_applied_ = true;
                Log(Info, "ReblurGhostingTest: nudge {} — yaw {:.1f} at frame {}", nudge_index_, current_yaw_,
                    frame_);
            }

            // Accumulate for AccumFrames before capturing
            if (frame_ < nudge_frame_ + AccumFrames)
            {
                return Result::Pending;
            }

            // Capture screenshot after accumulation
            auto name = std::format("ghosting_nudge_{}", nudge_index_);
            active_request_ = app.RequestTakeScreenshot(name);
            nudge_index_++;
            nudge_applied_ = false;
            return Result::Pending;
        }

        // Phase 3: Settle (hold camera still, reconverge)
        if (frame_ < nudge_frame_ + AccumFrames + SettleFrames)
        {
            return Result::Pending;
        }

        // Capture final settled screenshot
        if (!settled_captured_)
        {
            active_request_ = app.RequestTakeScreenshot("ghosting_settled");
            settled_captured_ = true;
            return Result::Pending;
        }

        Log(Info, "ReblurGhostingTest: all {} screenshots captured — PASS", NudgeCount + 2);
        return Result::Pass;
    }

private:
    static constexpr uint32_t WarmupFrames = 30;
    static constexpr uint32_t NudgeCount = 5;
    static constexpr float YawStep = 3.0f;  // degrees per nudge
    static constexpr uint32_t AccumFrames = 10;
    static constexpr uint32_t SettleFrames = 30;

    float initial_yaw_ = 0.0f;
    float current_yaw_ = 0.0f;
    uint32_t nudge_index_ = 0;
    uint32_t nudge_frame_ = 0;
    bool nudge_applied_ = false;
    bool before_captured_ = false;
    bool settled_captured_ = false;
    std::shared_ptr<ScreenshotRequest> active_request_;
};

static TestCaseRegistrar<ReblurGhostingTest> reblur_ghosting_registrar("reblur_ghosting");
} // namespace sparkle
