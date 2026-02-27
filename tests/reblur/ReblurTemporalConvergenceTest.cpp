#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"

namespace sparkle
{

/// Validates that REBLUR temporal pipeline (TemporalAccumulation + HistoryFix)
/// runs without crash over many frames with history buffers cycling.
///
/// Exercises the full temporal code path: PrePass -> TemporalAccum -> HistoryFix
/// -> Blur -> PostBlur across 30+ frames, ensuring history read/write and
/// internal_data ping-pong work correctly.
///
/// Usage: --test_case reblur_temporal_convergence --pipeline gpu --use_reblur true --spp 1 --max_spp 64
class ReblurTemporalConvergenceTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        // Log progress at key milestones
        if (frame_ == 1)
        {
            Log(Info, "ReblurTemporalConvergenceTest: frame 1 — first frame (reset_history)");
        }
        else if (frame_ == 2)
        {
            Log(Info, "ReblurTemporalConvergenceTest: frame 2 — first frame with valid history");
        }
        else if (frame_ == ConvergenceFrames)
        {
            Log(Info, "ReblurTemporalConvergenceTest: frame {} — convergence target reached", frame_);
        }

        // Wait for temporal convergence
        if (frame_ < ConvergenceFrames)
        {
            return Result::Pending;
        }

        if (request_ && request_->IsCompleted())
        {
            Log(Info, "ReblurTemporalConvergenceTest: screenshot captured after {} frames — PASS", frame_);
            return Result::Pass;
        }

        if (!request_)
        {
            Log(Info, "ReblurTemporalConvergenceTest: requesting screenshot at frame {}", frame_);
            request_ = app.RequestTakeScreenshot("reblur_temporal_convergence");
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t ConvergenceFrames = 30;
    std::shared_ptr<ScreenshotRequest> request_;
};

static TestCaseRegistrar<ReblurTemporalConvergenceTest> reblur_temporal_convergence_registrar(
    "reblur_temporal_convergence");
} // namespace sparkle
