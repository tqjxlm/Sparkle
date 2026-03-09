#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"

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
/// Usage: --test_case motion_luminance_track --pipeline gpu --use_reblur true --max_spp 60 --spp 1
/// Note: camera motion removed (CameraAnimator deleted). This test now runs static.
class MotionLuminanceTrackTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
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
            if (frame_ >= target_frame)
            {
                auto name = std::format("motion_track_{}", captured_count_);
                active_request_ = app.GetRenderFramework()->RequestTakeScreenshot(name);
                Log(Info, "{}: requesting '{}' at frame {} (target {})", GetName(), name, frame_, target_frame);
            }
            return Result::Pending;
        }

        // Phase 2: Wait for convergence after motion ends, then capture settled frame
        if (app.GetRenderFramework()->IsReadyForAutoScreenshot())
        {
            auto name = std::format("motion_track_settled");
            active_request_ = app.GetRenderFramework()->RequestTakeScreenshot(name);
            Log(Info, "{}: requesting settled screenshot at frame {}", GetName(), frame_);
            return Result::Pending;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t CaptureInterval = 5; // every 5 frames
    static constexpr uint32_t MotionCaptures = 5;  // 5 captures during motion (frames 5,10,15,20,25)
    static constexpr uint32_t TotalCaptures = 6;   // + 1 settled capture
    uint32_t captured_count_ = 0;
    std::shared_ptr<ScreenshotRequest> active_request_;
};

static TestCaseRegistrar<MotionLuminanceTrackTest> motion_luminance_track_registrar("motion_luminance_track");
} // namespace sparkle
