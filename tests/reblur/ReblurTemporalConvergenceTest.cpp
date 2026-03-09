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
/// Usage: --test_case reblur_temporal_convergence
class ReblurTemporalConvergenceTest : public TestCase
{
public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("pipeline", std::string("gpu"));
        EnforceConfig("spp", 1u);
    }

    Result OnTick(AppFramework &app) override
    {
        // Log progress at key milestones
        if (frame_ == 1)
        {
            Log(Info, "{}: frame 1 — first frame (reset_history)", GetName());
        }
        else if (frame_ == 2)
        {
            Log(Info, "{}: frame 2 — first frame with valid history", GetName());
        }
        else if (frame_ == ConvergenceFrames)
        {
            Log(Info, "{}: frame {} — convergence target reached", GetName(), frame_);
        }

        // Wait for temporal convergence
        if (frame_ < ConvergenceFrames)
        {
            return Result::Pending;
        }

        if (request_ && request_->IsCompleted())
        {
            Log(Info, "{}: screenshot captured after {} frames — PASS", GetName(), frame_);
            return Result::Pass;
        }

        if (!request_)
        {
            Log(Info, "{}: requesting screenshot at frame {}", GetName(), frame_);
            request_ = app.GetRenderFramework()->RequestTakeScreenshot("reblur_temporal_convergence");
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
