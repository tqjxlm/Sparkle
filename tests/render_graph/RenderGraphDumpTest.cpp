#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/FileManager.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string_view>

namespace sparkle
{
// writes the render graph of a frame rendered once the scene is ready for a screenshot to
// screenshots/render_graph.json, for tests/render_graph/graph_shape_test.py to compare against the pipeline's golden
// graph shape
class RenderGraphDumpTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (request_ && request_->IsCompleted())
        {
            if (AcceptsDump())
            {
                return Result::Pass;
            }
            request_.reset();
        }

        if (!request_ && IsReady(*app.GetRenderFramework()))
        {
            request_ = app.GetRenderFramework()->RequestGraphDump("render_graph");
        }

        return Result::Pending;
    }

protected:
    [[nodiscard]] virtual bool IsReady(const RenderFramework &render_framework) const
    {
        return render_framework.IsReadyForAutoScreenshot();
    }

    // whether the frame just dumped is the one to compare; otherwise the next frame is dumped
    [[nodiscard]] virtual bool AcceptsDump() const
    {
        return true;
    }

private:
    std::shared_ptr<ScreenshotRequest> request_;
};

// dumps a frame of the gpu pipeline that traces into an accumulator already holding samples, with no acceleration
// structure build pending: converged frames skip the trace, and the frames around the end of loading clear the
// accumulator or build the TLAS. the dumped frame is checked for that shape, so which frame it is does not depend on
// when loading finished.
class RenderGraphDumpAccumulatingTest : public RenderGraphDumpTest
{
protected:
    [[nodiscard]] bool IsReady(const RenderFramework &render_framework) const override
    {
        return render_framework.IsSceneFullyLoaded();
    }

    [[nodiscard]] bool AcceptsDump() const override
    {
        const auto dump = nlohmann::json::parse(FileManager::GetNativeFileManager()->ReadAsType<std::string>(
            Path::External("screenshots/render_graph.json")));
        const auto runs = [&dump](std::string_view pass) {
            return std::ranges::any_of(dump.at("passes"), [pass](const nlohmann::json &dumped) {
                return dumped.at("name").get_ref<const std::string &>() == pass && !dumped.at("culled").get<bool>();
            });
        };

        const bool accepted = runs("PathTrace") && !runs("ClearAccumulator") && !runs("BuildTLAS");
        if (!accepted)
        {
            Log(Info, "{}: the dumped frame does not accumulate onto earlier samples, dumping the next", GetName());
        }
        return accepted;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 1000;
    }
};

static TestCaseRegistrar<RenderGraphDumpTest> render_graph_dump_test_registrar("render_graph_dump");
static TestCaseRegistrar<RenderGraphDumpAccumulatingTest> accumulating_registrar("render_graph_dump_accumulating");
} // namespace sparkle
