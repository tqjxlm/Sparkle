#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/FileManager.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace sparkle
{
// runs windowed, where the ui draws. screenshots a frame with its ui to screenshots/render_graph_ui.png and dumps its
// render graph to screenshots/render_graph_ui.json, then switches to the cpu pipeline, whose ui draws against a screen
// of another format, and does both again as render_graph_ui_cpu. each graph must draw the ui, in the physical pass of
// the raster pass before it, or in a physical pass of its own after a pass of another kind.
class RenderGraphUiTest : public TestCase
{
public:
    static constexpr uint32_t SettleFrames = 30;

    void OnEnforceConfigs() override
    {
        EnforceConfig("headless", false);
    }

    Result OnTick(AppFramework &app) override
    {
        auto *render_framework = app.GetRenderFramework();
        if (!screenshot_)
        {
            // the cpu frame's noise does not matter, and converging it would take minutes
            if (frame_ >= settle_frame_ && (name_ == CpuName || render_framework->IsReadyForAutoScreenshot()))
            {
                screenshot_ = render_framework->RequestTakeScreenshot(name_, true);
                dump_ = render_framework->RequestGraphDump(name_);
            }
            return Result::Pending;
        }

        if (!screenshot_->IsCompleted() || !dump_->IsCompleted())
        {
            return Result::Pending;
        }

        CheckUiPhysicalPass();
        if (name_ != CpuName)
        {
            EnforceConfig("pipeline", std::string("cpu"));
            name_ = CpuName;
            settle_frame_ = frame_ + SettleFrames;
            screenshot_.reset();
            return Result::Pending;
        }
        return HasFailed() ? Result::Fail : Result::Pass;
    }

private:
    static constexpr const char *CpuName = "render_graph_ui_cpu";

    void CheckUiPhysicalPass()
    {
        const auto dump = nlohmann::json::parse(FileManager::GetNativeFileManager()->ReadAsType<std::string>(
            Path::External("screenshots/" + name_ + ".json")));

        const nlohmann::json *previous = nullptr;
        for (const auto &pass : dump.at("passes"))
        {
            if (pass.at("culled").get<bool>())
            {
                continue;
            }
            if (pass.at("name") == "Ui" && previous)
            {
                const auto &physical = previous->at("physical_pass");
                if (previous->at("kind") == "Raster")
                {
                    Expect(pass.at("physical_pass") == physical,
                           name_ + ": the ui draws in the physical pass of the raster pass before it");
                }
                else
                {
                    const auto reason = dump.at("physical_passes").at(physical.get<size_t>()).value("break_reason", "");
                    Expect(pass.at("physical_pass") != physical && reason == "NonRasterPass",
                           name_ + ": the ui starts a physical pass after the NonRasterPass break before it");
                }
                return;
            }
            previous = &pass;
        }
        Expect(false, name_ + ": the frame draws the ui");
    }

    std::string name_ = "render_graph_ui";
    uint32_t settle_frame_ = 0;
    std::shared_ptr<ScreenshotRequest> screenshot_;
    std::shared_ptr<ScreenshotRequest> dump_;
};

static TestCaseRegistrar<RenderGraphUiTest> render_graph_ui_test_registrar("render_graph_ui");
} // namespace sparkle
