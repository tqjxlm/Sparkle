#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"

#include <string>

namespace sparkle
{
// renderer recreation restarts the imgui backend; repeated switches must not lose the UI
class UiPipelineSwitchTest : public TestCase
{
    static constexpr uint32_t SettleFrames = 15;
    static constexpr uint32_t MaxSwitches = 40;

public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("pipeline", std::string("forward"));
    }

    Result OnTick(AppFramework &app) override
    {
        if (!app.GetRenderFramework()->IsSceneFullyLoaded())
        {
            return Result::Pending;
        }

        app.SetControlPanelVisible(true);

        if (frame_ < wait_until_frame_)
        {
            return Result::Pending;
        }

        if (switches_ >= MaxSwitches)
        {
            Log(Info, "{}: completed {} pipeline switches", GetName(), switches_);
            return Result::Pass;
        }

        const char *target = (switches_ % 2 == 0) ? "deferred" : "forward";
        EnforceConfig("pipeline", std::string(target));
        ++switches_;
        wait_until_frame_ = frame_ + SettleFrames;
        return Result::Pending;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 5000;
    }

private:
    uint32_t wait_until_frame_ = 0;
    uint32_t switches_ = 0;
};

static TestCaseRegistrar<UiPipelineSwitchTest> ui_pipeline_switch_test_registrar("ui_pipeline_switch");
} // namespace sparkle
