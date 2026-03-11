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
///   2. Nudge loop: for each nudge, rotate yaw by the configured yaw step,
///      capture an
///      early frame after FastCaptureFrames, then hold still and capture a
///      same-pose settled reference after SettleFrames. Repeat NudgeCount times.
///
/// Screenshots are named "ghosting_before" (pre-motion baseline),
/// "ghosting_nudge_N_fast" (few frames after the nudge), and
/// "ghosting_nudge_N_settled" (same pose after re-convergence).
///
/// Usage: --test_case reblur_ghosting
class ReblurGhostingTest : public TestCase
{
public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("pipeline", std::string("gpu"));
        EnforceConfig("use_reblur", true);
        EnforceConfig("spp", 1u);
        EnforceConfig("max_spp", 400u);
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return DefaultTimeoutFrames;
    }

    Result OnTick(AppFramework &app) override
    {
        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
        }

        const float yaw_step = app.GetRenderConfig().reblur_ghosting_yaw_step;

        if (!sequence_started_)
        {
            if (frame_ <= StartupDelayFrames)
            {
                return Result::Pending;
            }

            sequence_started_ = true;
            sequence_frame_ = 0;
            Log(Info, "{}: starting ghosting sequence after {} startup frames", GetName(), StartupDelayFrames);
        }

        sequence_frame_++;

        auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);

        // Wait for pending screenshot
        if (active_request_)
        {
            if (active_request_->IsCompleted())
            {
                Log(Info, "{}: captured '{}' at frame {}", GetName(), active_request_->GetName(), frame_);
                active_request_.reset();
            }
            else
            {
                return Result::Pending;
            }
        }

        // Phase 1: Static warmup
        if (sequence_frame_ <= WarmupFrames)
        {
            if (sequence_frame_ == 1 && orbit)
            {
                // Record initial pose
                initial_yaw_ = orbit->GetYaw();
                current_yaw_ = initial_yaw_;
                Log(Info, "{}: initial yaw = {:.1f}", GetName(), initial_yaw_);
            }
            return Result::Pending;
        }

        // Capture "before" baseline screenshot (converged, pre-motion)
        if (!before_captured_)
        {
            active_request_ = app.GetRenderFramework()->RequestTakeScreenshot("ghosting_before");
            before_captured_ = true;
            Log(Info, "{}: capturing 'before' at frame {}", GetName(), frame_);
            return Result::Pending;
        }

        // Phase 2: Nudge loop
        if (nudge_index_ < NudgeCount)
        {
            if (phase_ == Phase::ApplyNudge)
            {
                current_yaw_ += yaw_step;
                if (orbit)
                {
                    orbit->Setup(orbit->GetCenter(), orbit->GetRadius(), orbit->GetPitch(), current_yaw_);
                }
                phase_start_frame_ = sequence_frame_;
                phase_ = Phase::CaptureFast;
                Log(Info, "{}: nudge {} — yaw {:.1f} at sequence frame {}", GetName(), nudge_index_, current_yaw_,
                    sequence_frame_);
                return Result::Pending;
            }

            if (phase_ == Phase::CaptureFast)
            {
                if (sequence_frame_ < phase_start_frame_ + FastCaptureFrames)
                {
                    return Result::Pending;
                }

                auto name = std::format("ghosting_nudge_{}_fast", nudge_index_);
                active_request_ = app.GetRenderFramework()->RequestTakeScreenshot(name);
                phase_start_frame_ = sequence_frame_;
                phase_ = Phase::CaptureSettled;
                return Result::Pending;
            }

            if (phase_ == Phase::CaptureSettled)
            {
                if (sequence_frame_ < phase_start_frame_ + SettleFrames)
                {
                    return Result::Pending;
                }

                auto name = std::format("ghosting_nudge_{}_settled", nudge_index_);
                active_request_ = app.GetRenderFramework()->RequestTakeScreenshot(name);
                nudge_index_++;
                phase_start_frame_ = sequence_frame_;
                phase_ = Phase::ApplyNudge;
                return Result::Pending;
            }
        }

        Log(Info, "{}: all {} screenshots captured — PASS", GetName(), 1 + NudgeCount * 2);
        return Result::Pass;
    }

private:
    enum class Phase : uint8_t
    {
        ApplyNudge,
        CaptureFast,
        CaptureSettled,
    };

    static constexpr uint32_t WarmupFrames = 30;
    static constexpr uint32_t StartupDelayFrames = 10;
    static constexpr uint32_t NudgeCount = 5;
    static constexpr uint32_t FastCaptureFrames = 2;
    static constexpr uint32_t SettleFrames = 30;
    static constexpr uint32_t BaselineCaptureFrames = 3;
    static constexpr uint32_t PerNudgeFrames = 34;
    static constexpr uint32_t ExpectedPassFrames =
        StartupDelayFrames + WarmupFrames + BaselineCaptureFrames + NudgeCount * PerNudgeFrames;
    static constexpr uint32_t TimeoutSlackFrames = 17;
    static constexpr uint32_t DefaultTimeoutFrames = ExpectedPassFrames + TimeoutSlackFrames;
    // The current state machine should finish at about frame 213:
    //   10 startup + 30 warmup + 3 baseline-capture frames + 5 * 34 per-nudge frames.
    // The inferred timeout lives here because the sequence is deterministic.

    float initial_yaw_ = 0.0f;
    float current_yaw_ = 0.0f;
    uint32_t nudge_index_ = 0;
    uint32_t phase_start_frame_ = 0;
    uint32_t sequence_frame_ = 0;
    bool before_captured_ = false;
    bool sequence_started_ = false;
    Phase phase_ = Phase::ApplyNudge;
    std::shared_ptr<ScreenshotRequest> active_request_;
};

static TestCaseRegistrar<ReblurGhostingTest> reblur_ghosting_registrar("reblur_ghosting");
} // namespace sparkle
