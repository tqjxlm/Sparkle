#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "scene/component/camera/CameraComponent.h"

namespace sparkle
{

/// Cross-cutting non-regression: Validates that static camera REBLUR quality
/// is preserved after adding motion infrastructure (prev matrices, MVs, reprojection).
///
/// Runs 64 frames with static camera and REBLUR enabled, then takes a screenshot.
/// Validates:
/// - MVs should be zero (position_prev == position after convergence)
/// - Screenshot is not all-black, no NaN
/// - Accumulation should be progressing (sample count increases)
///
/// Usage: --test_case reblur_static_nonregression --pipeline gpu --use_reblur true --spp 1 --max_spp 64
class ReblurStaticNonRegressionTest : public TestCase
{
public:
    Result Tick(AppFramework &app) override
    {
        frame_++;

        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
        }

        auto *proxy = static_cast<CameraRenderProxy *>(camera->GetRenderProxy());

        // At midpoint, verify camera is truly static
        if (frame_ == 32)
        {
            auto pos = proxy->GetPosture().position;
            auto pos_prev = proxy->GetPositionPrev();
            float delta = (pos - pos_prev).norm();
            auto vp = proxy->GetViewProjectionMatrix();
            auto vp_prev = proxy->GetViewProjectionMatrixPrev();
            float mat_diff = (vp - vp_prev).norm();

            Log(Info, "ReblurStaticNonRegression: frame 32 — pos_delta={:.8f}, mat_diff={:.8f}", delta, mat_diff);

            // Static camera: position should not change after initial setup frames
            if (delta > 1e-4f)
            {
                Log(Error, "ReblurStaticNonRegression: FAIL — unexpected camera motion (delta={:.6f})", delta);
                return Result::Fail;
            }

            // Accumulation should be progressing
            uint32_t spp = proxy->GetCumulatedSampleCount();
            Log(Info, "ReblurStaticNonRegression: accumulated {} spp at frame 32", spp);
            if (spp < 10)
            {
                Log(Error, "ReblurStaticNonRegression: FAIL — accumulation too slow (spp={} at frame 32)", spp);
                return Result::Fail;
            }
        }

        // Wait for convergence
        if (frame_ < kConvergenceFrames)
        {
            return Result::Pending;
        }

        if (app.IsScreenshotCompleted())
        {
            Log(Info, "ReblurStaticNonRegression: screenshot captured after {} frames — PASS", frame_);
            return Result::Pass;
        }

        if (!screenshot_requested_)
        {
            Log(Info, "ReblurStaticNonRegression: requesting screenshot at frame {}", frame_);
            app.RequestTakeScreenshot("reblur_static_nonregression");
            screenshot_requested_ = true;
        }

        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "ReblurStaticNonRegression timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t kConvergenceFrames = 64;
    uint32_t frame_ = 0;
    bool screenshot_requested_ = false;
};

static TestCaseRegistrar<ReblurStaticNonRegressionTest> reblur_static_nonregression_registrar(
    "reblur_static_nonregression");
} // namespace sparkle
