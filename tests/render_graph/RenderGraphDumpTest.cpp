#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/FileManager.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
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

// with render_graph_export, waits for the export and checks its dump: a profile only under
// render_graph_profile_frames, of that many frames, whose timings are consistent with each other and the dumped frame
class RenderGraphExportTest : public TestCase
{
public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("render_graph_export", true);
    }

    Result OnTick(AppFramework &app) override
    {
        const auto &request = app.GetGraphExport();
        if (!request || !request->IsCompleted())
        {
            return Result::Pending;
        }

        const auto dump = nlohmann::json::parse(FileManager::GetNativeFileManager()->ReadAsType<std::string>(
            Path::External("screenshots/" + request->GetName() + ".json")));
        const auto frames = app.GetRenderConfig().render_graph_profile_frames;
        if (frames == 0)
        {
            Expect(!dump.contains("profile"), "an export without render_graph_profile_frames has no profile");
        }
        else
        {
            Expect(dump.at("profile").at("frames") == frames, "the profile aggregates render_graph_profile_frames");
            CheckTimings(dump.at("passes"), "cpu_ms", frames, true);
            CheckTimings(dump.at("physical_passes"), "gpu_ms", frames, false);
        }
        return HasFailed() ? Result::Fail : Result::Pass;
    }

private:
    // with `every_frame`, an entry timed in the dumped frame must be timed in every profiled frame
    void CheckTimings(const nlohmann::json &entries, const char *key, uint32_t frames, bool every_frame)
    {
        for (auto index = 0u; index < entries.size(); index++)
        {
            const auto &entry = entries.at(index);
            const bool timed = entry.contains(key);
            if (!entry.contains("profile"))
            {
                if (timed)
                {
                    Expect(false, std::format("entry {}, timed in the dumped frame, has a {} profile", index, key));
                }
                continue;
            }

            const auto &timing = entry.at("profile").at(key);
            const auto samples = timing.at("samples").get<uint32_t>();
            const auto mean = timing.at("mean").get<double>();
            const auto min = timing.at("min").get<double>();
            const auto max = timing.at("max").get<double>();
            // the mean of equal samples may round past them
            const auto tolerance = 1e-9 * std::max(1.0, max);
            const auto last = timed ? entry.at(key).get<double>() : min;
            Expect(samples >= 1 && samples <= frames && min <= mean + tolerance && mean <= max + tolerance &&
                       min <= last && last <= max && (!every_frame || !timed || samples == frames),
                   std::format("the {} profile {} of entry {} is consistent with its {} frames and the dumped frame",
                               key, timing.dump(), index, frames));
        }
    }
};

static TestCaseRegistrar<RenderGraphDumpTest> render_graph_dump_test_registrar("render_graph_dump");
static TestCaseRegistrar<RenderGraphDumpAccumulatingTest> accumulating_registrar("render_graph_dump_accumulating");
static TestCaseRegistrar<RenderGraphExportTest> render_graph_export_test_registrar("render_graph_export");
} // namespace sparkle
