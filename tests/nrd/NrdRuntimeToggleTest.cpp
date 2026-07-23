#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "renderer/denoiser/DenoiserConfig.h"

#include <format>
#include <memory>

namespace sparkle
{
// Guards the control-panel flow: NRD resources created at runtime inherit recycled device memory
// that one history reset cannot purge. The starting pipeline selects the arm — forward: runtime
// switch to gpu, then enable NRD mid-accumulation (memory-recycling repro); gpu: enable on a
// converged frame (render-freeze arm).
//
// Usage: --test_case nrd_runtime_toggle --pipeline [forward|gpu] --denoiser off
class NrdRuntimeToggleTest : public TestCase
{
public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("denoiser", std::string("off"));
    }

    Result OnTick(AppFramework &app) override
    {
        auto *rf = app.GetRenderFramework();

        switch (phase_)
        {
        case Phase::Init: {
            if (!rf->IsSceneFullyLoaded())
            {
                return Result::Pending;
            }
            if (DenoiserConfig::Get().provider != DenoiserProvider::Off)
            {
                Log(Error, "{}: a denoiser is already enabled; run with --denoiser off", GetName());
                return Result::Fail;
            }
            auto *pipeline = ConfigManager::Instance().GetConfig<std::string>("pipeline");
            switch_pipeline_ = pipeline->Get() != "gpu";
            phase_ = switch_pipeline_ ? Phase::SwitchPipeline : Phase::WaitConverged;
            return Result::Pending;
        }

        case Phase::SwitchPipeline:
            if (++tick_ < ForwardWarmupFrames)
            {
                return Result::Pending;
            }
            tick_ = 0;
            EnforceConfig("pipeline", std::string("gpu"));
            Log(Info, "{}: switched pipeline to gpu at runtime", GetName());
            phase_ = Phase::Accumulate;
            return Result::Pending;

        case Phase::Accumulate:
            if (++tick_ < AccumulateFrames)
            {
                return Result::Pending;
            }
            tick_ = 0;
            EnableNrd();
            return Result::Pending;

        case Phase::WaitConverged:
            if (!rf->IsReadyForAutoScreenshot())
            {
                return Result::Pending;
            }
            EnableNrd();
            return Result::Pending;

        case Phase::Capture:
            if (request_)
            {
                if (request_->IsCompleted())
                {
                    request_.reset();
                    Log(Info, "{}: captured frame {}/{}", GetName(), capture_idx_ + 1, NumFrames);
                    capture_idx_++;
                    if (capture_idx_ >= NumFrames)
                    {
                        return Result::Pass;
                    }
                }
                return Result::Pending;
            }
            if (++tick_ >= HoldTicks)
            {
                tick_ = 0;
                request_ = rf->RequestTakeScreenshot(std::format("nrd_toggle_{}", capture_idx_));
            }
            return Result::Pending;

        default:
            return Result::Pending;
        }
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 200000;
    }

private:
    enum class Phase : uint8_t
    {
        Init,
        SwitchPipeline,
        Accumulate,
        WaitConverged,
        Capture
    };

    void EnableNrd()
    {
        EnforceConfig("denoiser", std::string("nrd"));
        Log(Info, "{}: NRD enabled at runtime", GetName());
        phase_ = Phase::Capture;
    }

    static constexpr uint32_t NumFrames = 12;
    static constexpr uint32_t HoldTicks = 2;
    static constexpr uint32_t ForwardWarmupFrames = 60;
    static constexpr uint32_t AccumulateFrames = 120;

    Phase phase_ = Phase::Init;
    bool switch_pipeline_ = false;
    uint32_t capture_idx_ = 0;
    uint32_t tick_ = 0;
    std::shared_ptr<ScreenshotRequest> request_;
};

static TestCaseRegistrar<NrdRuntimeToggleTest> nrd_runtime_toggle_registrar("nrd_runtime_toggle");
} // namespace sparkle
