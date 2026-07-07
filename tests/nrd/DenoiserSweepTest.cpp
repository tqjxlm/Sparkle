#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/ConfigValue.h"
#include "core/Logger.h"
#include "scene/component/camera/OrbitCameraComponent.h"

#include <format>
#include <memory>

namespace sparkle
{
// both steps 0 => static camera: same frame-precise consecutive capture, but measuring temporal
// stability at rest.
static ConfigValue<float> config_sweep_step("sweep_step_degrees", "yaw step per captured frame in denoiser_sweep",
                                            "test", 2.f, false);
// vertical motion arm: exercises the matrix-derived reprojection paths that yaw leaves untouched
static ConfigValue<float> config_sweep_pitch_step("sweep_pitch_step_degrees",
                                                  "pitch step per captured frame in denoiser_sweep", "test", 0.f,
                                                  false);
// static mode only: frames to let the accumulator converge before capturing (must stay below max_spp,
// or the deterministic render-freeze kicks in and the captures film a frozen frame)
static ConfigValue<uint32_t> config_sweep_settle("sweep_settle_frames",
                                                 "static mode: frames to accumulate before capturing", "test", 16,
                                                 false);
// After the sweep, hold the final pose until fully converged and capture 'denoiser_sweep_converged':
// ground truth at the exact pose of the last motion capture (the fidelity harness compares them).
static ConfigValue<bool> config_sweep_converged("sweep_capture_converged",
                                                "capture a converged frame at the final sweep pose", "test", false,
                                                false);

// Captures CONSECUTIVE frames under continuous camera rotation (frame-precise, no per-pose convergence
// wait) — the scenario the user actually looks at when judging "noisy / soft under motion". Warms up
// uncaptured so the captured frames are motion steady state, then captures NumFrames frames one small
// yaw step apart.
//
// Usage: --test_case denoiser_sweep --pipeline gpu --framework macos [--nrd true] --max_spp 1
class DenoiserSweepTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        auto *rf = app.GetRenderFramework();

        switch (phase_)
        {
        case Phase::Init: {
            // Static mode films the live accumulation transient, so it starts when the scene is loaded,
            // NOT when the accumulator caps: readiness and the deterministic render-freeze trigger on the
            // same condition (cumulated >= max_spp), so waiting for readiness can only capture a frozen frame.
            const bool ready = IsStaticSweep() ? rf->IsSceneFullyLoaded() : rf->IsReadyForAutoScreenshot();
            if (!ready)
            {
                return Result::Pending;
            }
            if (!CaptureBasePose(app))
            {
                return Result::Fail;
            }
            phase_ = Phase::Warmup;
            return Result::Pending;
        }

        case Phase::Warmup:
            if (IsStaticSweep())
            {
                if (++warmup_tick_ >= config_sweep_settle.Get())
                {
                    phase_ = Phase::Sweep;
                }
                return Result::Pending;
            }
            if (warmup_step_ >= WarmupSteps)
            {
                phase_ = Phase::Sweep;
                return Result::Pending;
            }
            if (warmup_tick_ == 0)
            {
                SetPose(app, warmup_step_ + 1);
            }
            if (++warmup_tick_ >= HoldTicks)
            {
                warmup_tick_ = 0;
                warmup_step_++;
            }
            return Result::Pending;

        case Phase::Sweep:
            if (request_)
            {
                if (request_->IsCompleted())
                {
                    request_.reset();
                    Log(Info, "{}: captured frame {}/{}", GetName(), capture_idx_ + 1, NumFrames);
                    capture_idx_++;
                    if (capture_idx_ >= NumFrames)
                    {
                        if (config_sweep_converged.Get())
                        {
                            phase_ = Phase::Converge;
                            return Result::Pending;
                        }
                        return Result::Pass;
                    }
                }
                return Result::Pending;
            }
            SetPose(app, WarmupSteps + capture_idx_ + 1);
            if (++sweep_tick_ >= HoldTicks)
            {
                sweep_tick_ = 0;
                request_ = rf->RequestTakeScreenshot(std::format("denoiser_sweep_{}", capture_idx_));
            }
            return Result::Pending;

        case Phase::Converge:
            if (request_)
            {
                if (request_->IsCompleted())
                {
                    request_.reset();
                    Log(Info, "{}: captured converged frame", GetName());
                    return Result::Pass;
                }
                return Result::Pending;
            }
            if (!rf->IsReadyForAutoScreenshot())
            {
                return Result::Pending;
            }
            request_ = rf->RequestTakeScreenshot("denoiser_sweep_converged");
            return Result::Pending;

        default:
            return Result::Pending;
        }
        return Result::Pending;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 200000;
    }

private:
    enum class Phase : uint8_t
    {
        Init,
        Warmup,
        Sweep,
        Converge
    };

    bool CaptureBasePose(AppFramework &app)
    {
        auto *orbit = dynamic_cast<OrbitCameraComponent *>(app.GetMainCamera());
        if (orbit == nullptr)
        {
            Log(Error, "{}: main camera is not an OrbitCameraComponent; cannot script motion", GetName());
            return false;
        }
        base_center_ = orbit->GetCenter();
        base_radius_ = orbit->GetRadius();
        base_pitch_ = orbit->GetPitch();
        base_yaw_ = orbit->GetYaw();
        return true;
    }

    static bool IsStaticSweep()
    {
        return config_sweep_step.Get() == 0.f && config_sweep_pitch_step.Get() == 0.f;
    }

    void SetPose(AppFramework &app, uint32_t step) const
    {
        // static mode must leave the camera untouched: Setup dirties the camera even with identical
        // values, which resets the accumulator AND pins time_seed (so frames become bit-identical,
        // hiding the very temporal behavior a static-stability run is trying to observe).
        if (IsStaticSweep())
        {
            return;
        }
        auto *orbit = dynamic_cast<OrbitCameraComponent *>(app.GetMainCamera());
        orbit->Setup(base_center_, base_radius_, base_pitch_ + static_cast<float>(step) * config_sweep_pitch_step.Get(),
                     base_yaw_ + static_cast<float>(step) * config_sweep_step.Get());
    }

    static constexpr uint32_t NumFrames = 16;
    static constexpr uint32_t WarmupSteps = 8;
    static constexpr uint32_t HoldTicks = 2; // ticks per step: lets a 1-spp frame render before capture

    Phase phase_ = Phase::Init;
    uint32_t capture_idx_ = 0;
    uint32_t warmup_step_ = 0;
    uint32_t warmup_tick_ = 0;
    uint32_t sweep_tick_ = 0;
    std::shared_ptr<ScreenshotRequest> request_;

    Vector3 base_center_;
    float base_radius_ = 0.f;
    float base_pitch_ = 0.f;
    float base_yaw_ = 0.f;
};

static TestCaseRegistrar<DenoiserSweepTest> denoiser_sweep_registrar("denoiser_sweep");
} // namespace sparkle
