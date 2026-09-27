#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "renderer/graph/RGTexturePool.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "rhi/RHI.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <format>
#include <functional>
#include <memory>

namespace sparkle
{
// builds synthetic render graphs, compares their compiled plans (culling, physical images, barriers, load/store with
// reasons) against expected dump summaries, and executes each one. under synchronization validation that proves the
// planned barriers order every access, including a pooled image reused by the next graph. two graphs read back a
// texture to prove the recorded passes ran and a draw binds the texture its pass declared.
class RenderGraphCompileTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        if (failed_.load(std::memory_order_acquire))
        {
            return Result::Fail;
        }

        if (step_ == Steps.size())
        {
            return Result::Pass;
        }

        auto config = app.GetRenderConfig();
        config.image_width = 64;
        config.image_height = 32;
        config.render_scale = 0.5f;
        config.render_graph_cull = true;

        task_pending_.store(true, std::memory_order_release);
        const bool last = step_ + 1 == Steps.size();
        TaskManager::RunInRenderThread([this, rhi = app.GetRHI(), config, step = Steps[step_], last] {
            (this->*step)(rhi, config);
            if (last || failed_.load(std::memory_order_acquire))
            {
                resources_ = {};
            }
            task_pending_.store(false, std::memory_order_release);
        });
        step_++;

        return Result::Pending;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 1000;
    }

private:
    using Step = void (RenderGraphCompileTest::*)(RHIContext *, const RenderConfig &);

    static constexpr RGTextureDesc Rgba8Output{.format = PixelFormat::R8G8B8A8Unorm, .size_class = RGSizeClass::Output};
    static constexpr RGTextureDesc Rgba8Scene{.format = PixelFormat::R8G8B8A8Unorm, .size_class = RGSizeClass::Scene};

    struct Resources
    {
        std::unique_ptr<RGTexturePool> reuse_pool;
        RHIResourceRef<RHIComputePass> trace_pass;
    };

    static std::vector<std::string> Summarize(const nlohmann::json &dump)
    {
        std::vector<std::string> lines;
        for (const auto &pass : dump.at("passes"))
        {
            const auto name = pass.at("name").get<std::string>();
            if (pass.at("culled").get<bool>())
            {
                lines.push_back(std::format("{}: culled ({})", name, pass.at("cull_reason").get<std::string>()));
                continue;
            }

            lines.push_back(std::format("{}: {}", name, pass.at("kind").get<std::string>()));
            for (const auto &barrier : pass.at("barriers"))
            {
                lines.push_back(std::format(
                    "  barrier {} {}->{} [{} -> {}]", barrier.at("resource").get<std::string>(),
                    barrier.at("from_layout").get<std::string>(), barrier.at("to_layout").get<std::string>(),
                    barrier.at("from").get<std::string>(), barrier.at("to").get<std::string>()));
            }
            for (const auto &attachment : pass.at("attachments"))
            {
                const auto &slot = attachment.at("slot");
                lines.push_back(std::format(
                    "  attachment {} slot {}: {} ({}) / {} ({})", attachment.at("resource").get<std::string>(),
                    slot.is_string() ? slot.get<std::string>() : std::to_string(slot.get<unsigned>()),
                    attachment.at("load").get<std::string>(), attachment.at("load_reason").get<std::string>(),
                    attachment.at("store").get<std::string>(), attachment.at("store_reason").get<std::string>()));
            }
        }

        for (const auto &resource : dump.at("resources"))
        {
            if (resource.at("kind") == "Transient")
            {
                lines.push_back(resource.contains("physical")
                                    ? std::format("{}: physical {}", resource.at("name").get<std::string>(),
                                                  resource.at("physical").get<unsigned>())
                                    : std::format("{}: no image", resource.at("name").get<std::string>()));
            }
        }
        return lines;
    }

    // compiles, checks the plan and records the graph into its own command buffer
    void Run(RHIContext *rhi, RenderGraph &graph, const std::vector<std::string> &expected, const std::string &what)
    {
        graph.Compile();

        const auto summary = Summarize(graph.Dump());
        if (summary != expected)
        {
            Log(Error, "{}: FAILED - {} plan. expected:", GetName(), what);
            for (const auto &line : expected)
            {
                Log(Error, "{}", line);
            }
            Log(Error, "got (full dump {}):", graph.Dump().dump());
            for (const auto &line : summary)
            {
                Log(Error, "{}", line);
            }
            failed_.store(true, std::memory_order_release);
        }

        rhi->BeginCommandBuffer();
        graph.Execute(*rhi->GetCommandContext());
        rhi->SubmitCommandBuffer();
    }

    void Expect(bool condition, const std::string &what)
    {
        if (condition)
        {
            Log(Info, "{}: OK - {}", GetName(), what);
            return;
        }

        Log(Error, "{}: FAILED - {}", GetName(), what);
        failed_.store(true, std::memory_order_release);
    }

    static RHIResourceRef<RHIImage> CreateImportImage(RHIContext *rhi, Vector2UInt size, const std::string &name)
    {
        return rhi->CreateImage({.format = PixelFormat::R8G8B8A8Unorm,
                                 .sampler = Rgba8Output.sampler,
                                 .width = size.x(),
                                 .height = size.y(),
                                 .usages = RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::Texture},
                                name);
    }

    void ClearSampleReadback(RHIContext *rhi, const RenderConfig &config)
    {
        auto readback = CreateReadbackBuffer(rhi, config);

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(pool, config);
            const auto a = graph.CreateTexture("A", Rgba8Output);
            const auto b = graph.CreateTexture("B", Rgba8Output);
            graph.AddRasterPass("Clear", [a](RGBuilder &builder) {
                builder.ColorWrite(a, 0, Vector4(1.f, 0.f, 0.f, 1.f));
                return [](RGRasterContext &) {};
            });
            graph.AddRasterPass("Sample", [a, b](RGBuilder &builder) {
                builder.Sampled(a);
                builder.ColorWrite(b, 0, Vector4(0.f, 1.f, 0.f, 1.f));
                return [](RGRasterContext &) {};
            });
            graph.AddCopyPass("Readback", [b, buffer = readback.get()](RGBuilder &builder) {
                builder.CopySrc(b);
                builder.SideEffect();
                return [b, buffer](RGCopyContext &context) { context.CopyToBuffer(b, buffer); };
            });

            Run(rhi, graph,
                {
                    "Clear: Raster",
                    "  barrier A Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                    "  attachment A slot 0: Clear (clear) / Store (read by Sample)",
                    "Sample: Raster",
                    "  barrier A ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                    "  barrier B Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                    "  attachment B slot 0: Clear (clear) / Store (read by Readback)",
                    "Readback: Copy",
                    "  barrier B ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                    "A: physical 0",
                    "B: physical 1",
                },
                "clear, sample, readback");
        }

        rhi->WaitForDeviceIdle();
        const auto *pixel = static_cast<const uint8_t *>(readback->Lock());
        Expect(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255,
               "the readback pass copies the texture the sample pass cleared");
        readback->UnLock();
    }

    [[nodiscard]] static RHIResourceRef<RHIBuffer> CreateReadbackBuffer(RHIContext *rhi, const RenderConfig &config)
    {
        const auto output = config.GetResolution().output;
        return rhi->CreateBuffer({.size = output.x() * output.y() * 4u,
                                  .usages = RHIBuffer::BufferUsage::TransferDst,
                                  .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                  .is_dynamic = false},
                                 "RenderGraphTestReadback");
    }

    // a screen quad pipeline created sampling a placeholder draws a cleared texture, which reaches the readback only
    // when the draw binds the texture its pass declared
    void DeclaredBinding(RHIContext *rhi, const RenderConfig &config)
    {
        auto placeholder = CreateImportImage(rhi, config.GetResolution().output, "RenderGraphTestPlaceholder");
        const auto quad = PipelinePass::Create<ScreenQuadPass>(
            config, rhi, placeholder, rhi->CreateRenderTarget({}, placeholder, nullptr, "RenderGraphTestQuadTarget"));
        auto readback = CreateReadbackBuffer(rhi, config);

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(pool, config);
            const auto a = graph.CreateTexture("A", Rgba8Output);
            const auto b = graph.CreateTexture("B", Rgba8Output);
            graph.AddRasterPass("Clear", [a](RGBuilder &builder) {
                builder.ColorWrite(a, 0, Vector4(1.f, 0.f, 0.f, 1.f));
                return [](RGRasterContext &) {};
            });
            quad->AddTo(graph, "Quad", a, b);
            graph.AddCopyPass("Readback", [b, buffer = readback.get()](RGBuilder &builder) {
                builder.CopySrc(b);
                builder.SideEffect();
                return [b, buffer](RGCopyContext &context) { context.CopyToBuffer(b, buffer); };
            });

            Run(rhi, graph,
                {
                    "Clear: Raster",
                    "  barrier A Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                    "  attachment A slot 0: Clear (clear) / Store (read by Quad)",
                    "Quad: Raster",
                    "  barrier A ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                    "  barrier B Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                    "  attachment B slot 0: DontCare (fully overwritten) / Store (read by Readback)",
                    "Readback: Copy",
                    "  barrier B ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                    "A: physical 0",
                    "B: physical 1",
                },
                "declared binding");
        }

        rhi->WaitForDeviceIdle();
        const auto *pixel = static_cast<const uint8_t *>(readback->Lock());
        Expect(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255,
               "the quad draws the texture its pass declared");
        readback->UnLock();
    }

    void CulledBranch(RHIContext *rhi, const RenderConfig &config)
    {
        auto out = CreateImportImage(rhi, config.GetResolution().output, "RenderGraphTestOut");
        const auto build = [&out](RenderGraph &graph) {
            const auto out_texture = graph.Import("Out", out);
            const auto source = graph.CreateTexture("SourceColor", Rgba8Output);
            const auto debug = graph.CreateTexture("DebugColor", Rgba8Output);
            graph.AddRasterPass("DebugSource", [source](RGBuilder &builder) {
                builder.ColorWrite(source, 0, Vector4(0.f, 0.f, 0.f, 1.f));
                return [](RGRasterContext &) {};
            });
            graph.AddRasterPass("DebugView", [source, debug](RGBuilder &builder) {
                builder.Sampled(source);
                builder.ColorWrite(debug, 0, Vector4(0.f, 0.f, 0.f, 1.f));
                return [](RGRasterContext &) {};
            });
            graph.AddRasterPass("Main", [out_texture](RGBuilder &builder) {
                builder.ColorWrite(out_texture, 0, Vector4(0.f, 0.f, 0.f, 1.f));
                return [](RGRasterContext &) {};
            });
        };

        {
            RGTexturePool pool(rhi);
            RenderGraph graph(pool, config);
            build(graph);
            Run(rhi, graph,
                {
                    "DebugSource: culled (unread outputs: SourceColor)",
                    "DebugView: culled (unread outputs: DebugColor)",
                    "Main: Raster",
                    "  barrier Out Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                    "  attachment Out slot 0: Clear (clear) / Store (imported)",
                    "SourceColor: no image",
                    "DebugColor: no image",
                },
                "culled branch");
            Expect(pool.GetImageCount() == 0, "culled transients get no image");
        }

        auto no_cull = config;
        no_cull.render_graph_cull = false;
        RGTexturePool pool(rhi);
        RenderGraph graph(pool, no_cull);
        build(graph);
        Run(rhi, graph,
            {
                "DebugSource: Raster",
                "  barrier SourceColor Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment SourceColor slot 0: Clear (clear) / Store (read by DebugView)",
                "DebugView: Raster",
                "  barrier SourceColor ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                "  barrier DebugColor Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment DebugColor slot 0: Clear (clear) / DontCare (no later reader)",
                "Main: Raster",
                "  barrier Out Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment Out slot 0: Clear (clear) / Store (imported)",
                "SourceColor: physical 0",
                "DebugColor: physical 1",
            },
            "culling disabled");
    }

    void IntraFrameReuse(RHIContext *rhi, const RenderConfig &config)
    {
        auto result = CreateImportImage(rhi, config.GetResolution().scene, "RenderGraphTestResult");
        RGTexturePool pool(rhi);
        RenderGraph graph(pool, config);
        constexpr RGTextureDesc Desc{.format = PixelFormat::RGBAFloat16, .size_class = RGSizeClass::Scene};
        const auto t1 = graph.CreateTexture("T1", Desc);
        const auto t2 = graph.CreateTexture("T2", Desc);
        const auto t3 = graph.CreateTexture("T3", Desc);
        const auto result_texture = graph.Import("Result", result);
        graph.AddRasterPass("WriteT1", [t1](RGBuilder &builder) {
            builder.ColorWrite(t1, 0, Vector4(0.f, 0.f, 0.f, 1.f));
            return [](RGRasterContext &) {};
        });
        graph.AddRasterPass("ReadT1", [t1, t3](RGBuilder &builder) {
            builder.Sampled(t1);
            builder.ColorWrite(t3, 0, Vector4(0.f, 0.f, 0.f, 1.f));
            return [](RGRasterContext &) {};
        });
        graph.AddRasterPass("WriteT2", [t2](RGBuilder &builder) {
            builder.ColorWrite(t2, 0, Vector4(0.f, 0.f, 0.f, 1.f));
            return [](RGRasterContext &) {};
        });
        graph.AddRasterPass("ReadT2", [t2, t3, result_texture](RGBuilder &builder) {
            builder.Sampled(t2);
            builder.Sampled(t3);
            builder.ColorWrite(result_texture, 0, Vector4(0.f, 0.f, 0.f, 1.f));
            return [](RGRasterContext &) {};
        });

        Run(rhi, graph,
            {
                "WriteT1: Raster",
                "  barrier T1 Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment T1 slot 0: Clear (clear) / Store (read by ReadT1)",
                "ReadT1: Raster",
                "  barrier T1 ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                "  barrier T3 Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment T3 slot 0: Clear (clear) / Store (read by ReadT2)",
                "WriteT2: Raster",
                "  barrier T2 Undefined->ColorOutput [ColorWrite|Sampled(Pixel) -> ColorWrite]",
                "  attachment T2 slot 0: Clear (clear) / Store (read by ReadT2)",
                "ReadT2: Raster",
                "  barrier T2 ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                "  barrier T3 ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                "  barrier Result Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment Result slot 0: Clear (clear) / Store (imported)",
                "T1: physical 0",
                "T2: physical 0",
                "T3: physical 1",
            },
            "intra-frame reuse");
        Expect(pool.GetStats().num_created == 2, "three transients share two images");
    }

    void ImportedSeeding(RHIContext *rhi, const RenderConfig &config)
    {
        auto history = CreateImportImage(rhi, config.GetResolution().output, "RenderGraphTestHistory");
        RGTexturePool pool(rhi);
        RenderGraph graph(pool, config);
        const auto history_texture = graph.Import("History", history);
        Expect(graph.Import("HistoryAgain", history) == history_texture,
               "importing an image again returns its first import");
        const auto color = graph.CreateTexture("Color", Rgba8Output);
        graph.AddRasterPass("SampleHistory", [history_texture, color](RGBuilder &builder) {
            builder.Sampled(history_texture);
            builder.ColorWrite(color, 0, Vector4(0.f, 0.f, 0.f, 1.f));
            builder.SideEffect();
            return [](RGRasterContext &) {};
        });
        graph.AddExternalPass("External", [history_texture](RGBuilder &builder) {
            builder.ColorWrite(history_texture, 0);
            return [history_texture](RGExternalContext &context) {
                // foreign code transitioning into the state the graph already put the image in
                context.GetImage(history_texture)
                    ->Transition({.target_layout = RHIImageLayout::ColorOutput,
                                  .after_stage = RHIPipelineStage::ColorOutput,
                                  .before_stage = RHIPipelineStage::ColorOutput});
            };
        });
        graph.AddRasterPass("WriteHistory", [history_texture](RGBuilder &builder) {
            builder.ColorWrite(history_texture, 0);
            return [](RGRasterContext &) {};
        });

        // the tracked state the graph starts from: sampled by pixel shaders
        rhi->BeginCommandBuffer();
        history->Transition({.target_layout = RHIImageLayout::Read,
                             .after_stage = RHIPipelineStage::ColorOutput,
                             .before_stage = RHIPipelineStage::PixelShader});
        rhi->SubmitCommandBuffer();

        Run(rhi, graph,
            {
                "SampleHistory: Raster",
                "  barrier Color Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment Color slot 0: Clear (clear) / DontCare (no later reader)",
                "External: External",
                "  barrier History Read->ColorOutput [ColorWrite|Sampled(Pixel) -> ColorWrite]",
                "WriteHistory: Raster",
                "  barrier History ColorOutput->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment History slot 0: Load (written by External) / Store (imported)",
                "Color: physical 0",
            },
            "imported seeding");
        Expect(history->GetState(0, 0) ==
                   RHIImageState{.layout = RHIImageLayout::ColorOutput, .access = {.access = RHIAccess::ColorWrite}},
               "the graph writes the final state through to the import");
    }

    void LoadStore(RHIContext *rhi, const RenderConfig &config)
    {
        auto output = CreateImportImage(rhi, config.GetResolution().scene, "RenderGraphTestOutput");
        RGTexturePool pool(rhi);
        RenderGraph graph(pool, config);
        const auto marker = graph.CreateTexture("MarkerColor", Rgba8Scene);
        const auto color = graph.CreateTexture("Color", Rgba8Scene);
        const auto depth = graph.CreateTexture("Depth", {.format = PixelFormat::D32, .size_class = RGSizeClass::Scene});
        const auto output_texture = graph.Import("Output", output);
        graph.AddRasterPass("Marker", [marker](RGBuilder &builder) {
            builder.ColorWrite(marker, 0);
            builder.SideEffect();
            return [](RGRasterContext &) {};
        });
        graph.AddRasterPass("Base", [color, depth](RGBuilder &builder) {
            builder.ColorWrite(color, 0, Vector4(0.f, 0.f, 0.f, 1.f));
            builder.DepthWrite(depth, 1.f);
            return [](RGRasterContext &) {};
        });
        graph.AddRasterPass("Overlay", [color, depth](RGBuilder &builder) {
            builder.ColorWrite(color, 0);
            builder.DepthTest(depth);
            builder.SideEffect();
            return [](RGRasterContext &) {};
        });
        graph.AddRasterPass("Fullscreen", [color](RGBuilder &builder) {
            builder.ColorWrite(color, 0);
            builder.FullyOverwrites();
            return [](RGRasterContext &) {};
        });
        graph.AddRasterPass("Compose", [color, output_texture](RGBuilder &builder) {
            builder.Sampled(color);
            builder.ColorWrite(output_texture, 0);
            return [](RGRasterContext &) {};
        });

        Run(rhi, graph,
            {
                "Marker: Raster",
                "  barrier MarkerColor Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment MarkerColor slot 0: DontCare (no earlier writer) / DontCare (no later reader)",
                "Base: Raster",
                "  barrier Color Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  barrier Depth Undefined->DepthStencilOutput [DepthWrite -> DepthWrite]",
                "  attachment Color slot 0: Clear (clear) / Store (read by Overlay)",
                "  attachment Depth slot depth: Clear (clear) / Store (read by Overlay)",
                "Overlay: Raster",
                "  barrier Color ColorOutput->ColorOutput [ColorWrite -> ColorWrite]",
                "  barrier Depth DepthStencilOutput->DepthStencilOutput [DepthWrite -> DepthWrite]",
                "  attachment Color slot 0: Load (written by Base) / DontCare (overwritten by Fullscreen)",
                "  attachment Depth slot depth: Load (written by Base) / DontCare (no later reader)",
                "Fullscreen: Raster",
                "  barrier Color Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment Color slot 0: DontCare (fully overwritten) / Store (read by Compose)",
                "Compose: Raster",
                "  barrier Color ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                "  barrier Output Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment Output slot 0: Load (imported) / Store (imported)",
                "MarkerColor: physical 0",
                "Color: physical 0",
                "Depth: physical 1",
            },
            "load/store inference");
    }

    // the same graph in two consecutive frames: the second reuses the first's images, and its barriers wait for the
    // accesses the first frame left in their tracked state
    void NextFrameReuse(RHIContext *rhi, const RenderConfig &config)
    {
        const bool first_frame = !resources_.reuse_pool;
        if (first_frame)
        {
            resources_.reuse_pool = std::make_unique<RGTexturePool>(rhi);
            resources_.trace_pass = rhi->CreateComputePass("RenderGraphTestTrace", false);
        }

        auto &pool = *resources_.reuse_pool;
        RenderGraph graph(pool, config);
        const auto radiance =
            graph.CreateTexture("Radiance", {.format = PixelFormat::R32Float, .size_class = RGSizeClass::Scene});
        const auto shown = graph.CreateTexture("Shown", Rgba8Scene);
        graph.AddComputePass("Trace", resources_.trace_pass, [radiance](RGBuilder &builder) {
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

        Run(rhi, graph,
            {
                "Trace: Compute",
                first_frame ? "  barrier Radiance Undefined->StorageWrite [None -> StorageWrite(Compute)]"
                            : "  barrier Radiance Undefined->StorageWrite [Sampled(Pixel) -> StorageWrite(Compute)]",
                "Show: Raster",
                "  barrier Radiance StorageWrite->Read [StorageWrite(Compute) -> Sampled(Pixel)]",
                "  barrier Shown Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  attachment Shown slot 0: Clear (clear) / DontCare (no later reader)",
                "Radiance: physical 0",
                "Shown: physical 1",
            },
            first_frame ? "first frame" : "next frame");

        const auto &stats = pool.GetStats();
        Expect(stats.num_created == 2 && stats.num_reused == (first_frame ? 0u : 2u),
               first_frame ? "the first frame creates its images"
                           : "the next frame reuses the previous frame's images");
    }

    void ReleaseUnused(RHIContext * /*rhi*/, const RenderConfig &config)
    {
        auto &pool = *resources_.reuse_pool;
        const auto run_empty_graph = [&pool, &config] {
            RenderGraph graph(pool, config);
            graph.Compile();
        };
        for (auto i = 1u; i < RGTexturePool::UnusedGraphsBeforeRelease; i++)
        {
            run_empty_graph();
        }
        Expect(pool.GetImageCount() == 2, "recently used images stay pooled");
        run_empty_graph();
        Expect(pool.GetImageCount() == 0 && pool.GetStats().num_released == 2,
               "images unused for UnusedGraphsBeforeRelease graphs are released");
    }

    // each step runs in its own frame, so the next-frame reuse steps are consecutive frames
    static constexpr std::array<Step, 9> Steps{
        &RenderGraphCompileTest::ClearSampleReadback, &RenderGraphCompileTest::DeclaredBinding,
        &RenderGraphCompileTest::CulledBranch,        &RenderGraphCompileTest::IntraFrameReuse,
        &RenderGraphCompileTest::ImportedSeeding,     &RenderGraphCompileTest::LoadStore,
        &RenderGraphCompileTest::NextFrameReuse,      &RenderGraphCompileTest::NextFrameReuse,
        &RenderGraphCompileTest::ReleaseUnused,
    };

    size_t step_ = 0;
    std::atomic<bool> task_pending_{false};
    std::atomic<bool> failed_{false};

    // only accessed from the render thread
    Resources resources_;
};

static TestCaseRegistrar<RenderGraphCompileTest> render_graph_compile_test_registrar("render_graph_compile");
} // namespace sparkle
