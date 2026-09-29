#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "renderer/graph/RGPassTimers.h"
#include "renderer/graph/RGTexturePool.h"
#include "renderer/graph/RenderGraph.h"
#include "rhi/RHI.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>

namespace sparkle
{
// executes a graph of a compute pass and a raster pass once per frame, with pass timers kept across frames, until its
// dump carries a GPU time for both passes once their frame slot comes back (0 accepted: some devices cannot resolve
// it). a device without pass timestamps must dump no time at all.
class RenderGraphPassTimingTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        if (done_.load(std::memory_order_acquire))
        {
            return failed_.load(std::memory_order_acquire) ? Result::Fail : Result::Pass;
        }

        task_pending_.store(true, std::memory_order_release);

        TaskManager::RunInRenderThread([this, rhi = app.GetRHI(), config = app.GetRenderConfig()] {
            if (!timers_)
            {
                Create(rhi);
            }

            RecordAndCheck(rhi, config);

            task_pending_.store(false, std::memory_order_release);
        });

        return Result::Pending;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 1000;
    }

private:
    // enough frames for every slot to come back several times
    static constexpr unsigned MaxRecordings = 32;

    static constexpr RGTextureDesc Radiance{
        .format = PixelFormat::R32Float, .size_class = RGSizeClass::Absolute, .width = 4, .height = 4};
    static constexpr RGTextureDesc Shown{
        .format = PixelFormat::R8G8B8A8Unorm, .size_class = RGSizeClass::Absolute, .width = 4, .height = 4};

    void Create(RHIContext *rhi)
    {
        pool_ = std::make_unique<RGTexturePool>(rhi);
        timers_ = std::make_unique<RGPassTimers>(rhi);
        compute_pass_ = rhi->CreateComputePass("RenderGraphPassTimingTestTrace", true);

        supported_ = rhi->SupportsPassTimestamps();
        Log(Info, "{}: pass timestamps supported: {}", GetName(), supported_);
    }

    // executes the graph of this frame and returns its dump
    [[nodiscard]] nlohmann::json ExecuteGraph(RHIContext *rhi, const RenderConfig &config)
    {
        RenderGraph graph(rhi, *pool_, config);
        const auto radiance = graph.CreateTexture("Radiance", Radiance);
        const auto shown = graph.CreateTexture("Shown", Shown);
        graph.AddComputePass("Trace", compute_pass_, [radiance](RGBuilder &builder) {
            builder.StorageWrite(radiance);
            builder.FullyOverwrites();
            return [](RGComputeContext &) {};
        });
        graph.AddRasterPass("Show", [radiance, shown](RGBuilder &builder) {
            builder.Sampled(radiance);
            builder.ColorWrite(shown, 0, Vector4(0.f, 0.f, 0.f, 1.f));
            builder.SideEffect();
            return [](RGRasterContext &) {};
        });
        graph.Compile();

        graph.Execute(rhi->BeginCommandBuffer(), timers_.get());
        rhi->SubmitCommandBuffer();

        return graph.Dump();
    }

    void RecordAndCheck(RHIContext *rhi, const RenderConfig &config)
    {
        const auto dump = ExecuteGraph(rhi, config);
        unsigned timed = 0;
        for (const auto &pass : dump.at("passes"))
        {
            if (pass.contains("gpu_ms"))
            {
                Log(Info, "{}: {} took {:.6f} ms", GetName(), pass.at("name").get<std::string>(),
                    pass.at("gpu_ms").get<float>());
                timed++;
            }
        }
        if (!supported_)
        {
            Expect(timed == 0, "graph passes report no time without pass timestamps");
        }

        recordings_++;
        if (supported_ && timed == 2)
        {
            Finish();
        }
        else if (recordings_ == MaxRecordings)
        {
            Expect(!supported_, "the raster and the compute pass report their GPU time");
            Finish();
        }
    }

    void Finish()
    {
        compute_pass_ = nullptr;
        timers_ = nullptr;
        pool_ = nullptr;
        done_.store(true, std::memory_order_release);
    }

    void Expect(bool condition, const std::string &what)
    {
        if (condition)
        {
            return;
        }

        Log(Error, "{}: FAILED - {}", GetName(), what);
        failed_.store(true, std::memory_order_release);
    }

    std::unique_ptr<RGTexturePool> pool_;
    std::unique_ptr<RGPassTimers> timers_;
    RHIResourceRef<RHIComputePass> compute_pass_;
    bool supported_ = false;
    unsigned recordings_ = 0;
    std::atomic<bool> task_pending_{false};
    std::atomic<bool> done_{false};
    std::atomic<bool> failed_{false};
};

static TestCaseRegistrar<RenderGraphPassTimingTest> render_graph_pass_timing_test_registrar("render_graph_pass_timing");
} // namespace sparkle
