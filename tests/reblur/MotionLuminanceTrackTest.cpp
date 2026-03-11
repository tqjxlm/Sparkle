#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"
#include "scene/component/camera/OrbitCameraComponent.h"

#include <format>

namespace sparkle
{

/// Captures screenshots at fixed frame intervals during continuous camera motion.
/// Does NOT wait for convergence — captures during active animation to track
/// luminance trends in recently disoccluded regions.
///
/// Captures at frames: 5, 10, 15, 20, 25 (during motion)
/// Then waits for convergence and captures one final "settled" screenshot.
///
/// Usage: --test_case motion_luminance_track
class MotionLuminanceTrackTest : public TestCase
{
public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("pipeline", std::string("gpu"));
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

        if (!sequence_started_)
        {
            if (frame_ <= StartupDelayFrames)
            {
                return Result::Pending;
            }

            sequence_started_ = true;
            sequence_frame_ = 0;
            Log(Info, "{}: starting motion capture sequence after {} warmup frames", GetName(), StartupDelayFrames);
        }

        sequence_frame_++;

        auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);
        if (!orbit)
        {
            Log(Error, "{}: requires OrbitCameraComponent main camera", GetName());
            return Result::Fail;
        }

        if (!motion_initialized_)
        {
            current_yaw_ = orbit->GetYaw();
            motion_initialized_ = true;
            Log(Info, "{}: starting yaw = {:.2f}", GetName(), current_yaw_);
        }

        if (sequence_frame_ <= MotionFrames)
        {
            current_yaw_ += YawStepPerFrame;
            orbit->Setup(orbit->GetCenter(), orbit->GetRadius(), orbit->GetPitch(), current_yaw_);
        }

        // Handle pending screenshot request
        if (active_request_)
        {
            if (active_request_->IsCompleted())
            {
                Log(Info, "{}: captured '{}' at frame {}", GetName(), active_request_->GetName(), frame_);
                active_request_.reset();
                captured_count_++;

                // All done?
                if (captured_count_ >= TotalCaptures)
                {
                    return Result::Pass;
                }
            }
            return Result::Pending;
        }

        // Phase 1: Capture during motion at fixed intervals
        if (captured_count_ < MotionCaptures)
        {
            uint32_t target_frame = (captured_count_ + 1) * CaptureInterval;
            if (sequence_frame_ >= target_frame)
            {
                auto name = std::format("motion_track_{}", captured_count_);
                active_request_ = app.GetRenderFramework()->RequestTakeScreenshot(name);
                Log(Info, "{}: requesting '{}' at sequence frame {} (target {})", GetName(), name, sequence_frame_,
                    target_frame);
            }
            return Result::Pending;
        }

        // Phase 2: Wait for convergence after motion ends, then capture settled frame
        if (app.GetRenderFramework()->IsReadyForAutoScreenshot())
        {
            auto name = std::format("motion_track_settled");
            active_request_ = app.GetRenderFramework()->RequestTakeScreenshot(name);
            Log(Info, "{}: requesting settled screenshot at sequence frame {}", GetName(), sequence_frame_);
            return Result::Pending;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t StartupDelayFrames = 30;
    static constexpr uint32_t CaptureInterval = 5; // every 5 frames
    static constexpr uint32_t MotionCaptures = 5;  // 5 captures during motion (frames 5,10,15,20,25)
    static constexpr uint32_t TotalCaptures = 6;   // + 1 settled capture
    static constexpr uint32_t MotionFrames = CaptureInterval * MotionCaptures;
    static constexpr float YawStepPerFrame = 1.5f;
    uint32_t captured_count_ = 0;
    uint32_t sequence_frame_ = 0;
    float current_yaw_ = 0.0f;
    bool motion_initialized_ = false;
    bool sequence_started_ = false;
    std::shared_ptr<ScreenshotRequest> active_request_;
};

static TestCaseRegistrar<MotionLuminanceTrackTest> motion_luminance_track_registrar("motion_luminance_track");
} // namespace sparkle
