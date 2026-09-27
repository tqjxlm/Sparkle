#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"

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
            return Result::Pass;
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

private:
    std::shared_ptr<ScreenshotRequest> request_;
};

// dumps a frame rendered once the scene is loaded, while a progressive renderer still accumulates: its converged
// frames skip the passes that accumulate
class RenderGraphDumpAccumulatingTest : public RenderGraphDumpTest
{
protected:
    [[nodiscard]] bool IsReady(const RenderFramework &render_framework) const override
    {
        return render_framework.IsSceneFullyLoaded();
    }
};

static TestCaseRegistrar<RenderGraphDumpTest> render_graph_dump_test_registrar("render_graph_dump");
static TestCaseRegistrar<RenderGraphDumpAccumulatingTest> accumulating_registrar("render_graph_dump_accumulating");
} // namespace sparkle
