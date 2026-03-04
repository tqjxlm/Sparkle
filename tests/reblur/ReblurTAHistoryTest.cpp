#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"

namespace sparkle
{

/// Validates that TAHistory debug mode shows history that improves over time.
///
/// The TAHistory debug pass outputs the raw reprojected history buffer contents
/// (before temporal blending). If history is being updated properly, the output
/// at frame 60 should be significantly less noisy than at frame 10, because the
/// history contains accumulated (converging) data from prior frames.
///
/// If the history feedback loop is broken (diagnostic data fed back as history),
/// the output noise level will NOT decrease over time.
///
/// Phase 1: Wait 10 frames, take "early" screenshot.
/// Phase 2: Continue to frame 60, take "late" screenshot.
///
/// Python companion (test_ta_history.py) validates noise decreases.
///
/// Usage: --test_case reblur_ta_history --pipeline gpu --use_reblur true --spp 1
///        --max_spp 64 --reblur_debug_pass TAHistory
class ReblurTAHistoryTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (frame_ == 1)
        {
            Log(Info, "ReblurTAHistoryTest: started (TAHistory debug mode)");
        }

        // Phase 1: Take "early" screenshot at frame 10
        if (frame_ == EarlyFrame && !early_request_)
        {
            Log(Info, "ReblurTAHistoryTest: requesting 'early' screenshot at frame {}", frame_);
            early_request_ = app.RequestTakeScreenshot("ta_history_early");
            return Result::Pending;
        }

        if (early_request_ && !early_done_ && early_request_->IsCompleted())
        {
            early_done_ = true;
            Log(Info, "ReblurTAHistoryTest: 'early' screenshot captured");
        }

        if (frame_ >= EarlyFrame && !early_done_)
        {
            return Result::Pending;
        }

        // Phase 2: Take "late" screenshot at frame 60
        if (frame_ == LateFrame && !late_request_)
        {
            Log(Info, "ReblurTAHistoryTest: requesting 'late' screenshot at frame {}", frame_);
            late_request_ = app.RequestTakeScreenshot("ta_history_late");
            return Result::Pending;
        }

        if (late_request_ && late_request_->IsCompleted())
        {
            Log(Info, "ReblurTAHistoryTest: 'late' screenshot captured at frame {} — PASS", frame_);
            return Result::Pass;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t EarlyFrame = 10;
    static constexpr uint32_t LateFrame = 60;

    std::shared_ptr<ScreenshotRequest> early_request_;
    std::shared_ptr<ScreenshotRequest> late_request_;
    bool early_done_ = false;
};

static TestCaseRegistrar<ReblurTAHistoryTest> reblur_ta_history_registrar("reblur_ta_history");
} // namespace sparkle
