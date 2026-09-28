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
// planned barriers order every access, including a pooled image reused by the next graph and a buffer copied through.
// graphs read back a texture to prove the recorded passes ran and a draw binds the texture and sampler its pass
// declared.
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

    // the resource an entry names, with its subresources unless it covers every one
    static std::string GetResource(const nlohmann::json &entry)
    {
        const auto resource = entry.at("resource").get<std::string>();
        return entry.contains("subresources")
                   ? std::format("{}[{}]", resource, entry.at("subresources").get<std::string>())
                   : resource;
    }

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
                // memory barriers have no layouts
                const auto layouts = barrier.contains("from_layout")
                                         ? std::format(" {}->{}", barrier.at("from_layout").get<std::string>(),
                                                       barrier.at("to_layout").get<std::string>())
                                         : std::string();
                lines.push_back(std::format("  barrier {}{} [{} -> {}]", GetResource(barrier), layouts,
                                            barrier.at("from").get<std::string>(),
                                            barrier.at("to").get<std::string>()));
            }
            for (const auto &attachment : pass.at("attachments"))
            {
                const auto &slot = attachment.at("slot");
                lines.push_back(std::format(
                    "  attachment {} slot {}: {} ({}) / {} ({})", GetResource(attachment),
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
            AddReadback(graph, b, readback);

            Run(rhi, graph,
                {
                    "Clear: Raster",
                    "  barrier A Undefined->ColorOutput [None -> ColorWrite]",
                    "  attachment A slot 0: Clear (clear) / Store (read by Sample)",
                    "Sample: Raster",
                    "  barrier A ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                    "  barrier B Undefined->ColorOutput [None -> ColorWrite]",
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

    // copies `texture` into the imported `readback` buffer
    static void AddReadback(RenderGraph &graph, RGTexture texture, const RHIResourceRef<RHIBuffer> &readback)
    {
        graph.AddCopyPass("Readback", [texture, buffer = graph.Import("Readback", readback)](RGBuilder &builder) {
            builder.CopySrc(texture);
            builder.CopyDst(buffer);
            return [texture, buffer](RGCopyContext &context) { context.CopyToBuffer(texture, buffer); };
        });
    }

    // a texture copied into a device buffer and back into another texture, which reuses the first one's image: the
    // buffer's memory barrier orders the copies, and its tracked access is written through
    void BufferRoundTrip(RHIContext *rhi, const RenderConfig &config)
    {
        const auto output = config.GetResolution().output;
        auto staging =
            rhi->CreateBuffer({.size = output.x() * output.y() * 4u,
                               .usages = RHIBuffer::BufferUsage::TransferSrc | RHIBuffer::BufferUsage::TransferDst,
                               .mem_properties = RHIMemoryProperty::DeviceLocal,
                               .is_dynamic = false},
                              "RenderGraphTestStaging");
        auto readback = CreateReadbackBuffer(rhi, config);

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(pool, config);
            const auto a = graph.CreateTexture("A", Rgba8Output);
            const auto b = graph.CreateTexture("B", Rgba8Output);
            const auto buffer = graph.Import("Staging", staging);
            graph.AddRasterPass("Clear", [a](RGBuilder &builder) {
                builder.ColorWrite(a, 0, Vector4(0.f, 0.f, 1.f, 1.f));
                return [](RGRasterContext &) {};
            });
            graph.AddCopyPass("Download", [a, buffer](RGBuilder &builder) {
                builder.CopySrc(a);
                builder.CopyDst(buffer);
                return [a, buffer](RGCopyContext &context) { context.CopyToBuffer(a, buffer); };
            });
            graph.AddCopyPass("Upload", [buffer, b](RGBuilder &builder) {
                builder.CopySrc(buffer);
                builder.CopyDst(b);
                builder.FullyOverwrites();
                return [buffer, b](RGCopyContext &context) { context.CopyFromBuffer(buffer, b); };
            });
            AddReadback(graph, b, readback);

            Run(rhi, graph,
                {
                    "Clear: Raster",
                    "  barrier A Undefined->ColorOutput [None -> ColorWrite]",
                    "  attachment A slot 0: Clear (clear) / Store (read by Download)",
                    "Download: Copy",
                    "  barrier A ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                    "Upload: Copy",
                    "  barrier B Undefined->TransferDst [CopySrc -> CopyDst]",
                    "  barrier Staging [CopyDst -> CopySrc]",
                    "Readback: Copy",
                    "  barrier B TransferDst->TransferSrc [CopyDst -> CopySrc]",
                    "A: physical 0",
                    "B: physical 0",
                },
                "buffer round trip");

            const auto resources = graph.Dump().at("resources");
            const auto &staging_dump = resources.at(2);
            Expect(resources.at(0).at("type") == "Texture" && staging_dump.at("type") == "Buffer" &&
                       staging_dump.at("first_use") == 1 && staging_dump.at("last_use") == 2,
                   "the dump names each resource's type and the passes that first and last use it by index");
        }

        Expect(staging->GetTracked().GetTrackedAccess() == RHIResourceAccess{.access = RHIAccess::CopySrc},
               "the graph writes the final access through to the buffer");

        rhi->WaitForDeviceIdle();
        const auto *pixel = static_cast<const uint8_t *>(readback->Lock());
        Expect(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255,
               "the texture copied through the buffer is read back");
        readback->UnLock();
    }

    // a screen quad pipeline created from an attachment signature, with nothing bound to sample, draws a cleared
    // texture, which reaches the readback only when the draw binds the texture and sampler its pass declared
    void DeclaredBinding(RHIContext *rhi, const RenderConfig &config)
    {
        const auto quad = PipelinePass::Create<ScreenQuadPass>(config, rhi, "Quad", Rgba8Output.format);
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
            quad->AddTo(graph, a, b);
            AddReadback(graph, b, readback);

            Run(rhi, graph,
                {
                    "Clear: Raster",
                    "  barrier A Undefined->ColorOutput [None -> ColorWrite]",
                    "  attachment A slot 0: Clear (clear) / Store (read by Quad)",
                    "Quad: Raster",
                    "  barrier A ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                    "  barrier B Undefined->ColorOutput [None -> ColorWrite]",
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

    // a face of one mip cleared, another mip written and the face sampled, then the whole cube sampled: each access
    // plans barriers for its own subresources, the clear is stored for the reader of its face rather than dropped for
    // the writer of the other mip, and the face already sampled needs no second barrier
    void Subresources(RHIContext *rhi, const RenderConfig &config)
    {
        auto cube = rhi->CreateImage({.format = PixelFormat::R8G8B8A8Unorm,
                                      .sampler = Rgba8Output.sampler,
                                      .width = 8,
                                      .height = 8,
                                      .usages = RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::UAV |
                                                RHIImage::ImageUsage::Texture,
                                      .mip_levels = 2,
                                      .type = RHIImage::ImageType::Image2DCube},
                                     "RenderGraphTestCube");
        const auto compute_pass = rhi->CreateComputePass("RenderGraphTestCompute", false);

        RGTexturePool pool(rhi);
        RenderGraph graph(pool, config);
        const auto cube_texture = graph.Import("Cube", cube);
        graph.AddRasterPass("ClearFace", [cube_texture](RGBuilder &builder) {
            builder.ColorWrite(cube_texture.Subresource(1, 2), 0, Vector4(0.f, 0.f, 0.f, 1.f));
            return [](RGRasterContext &) {};
        });
        graph.AddComputePass("WriteMip", compute_pass, [cube_texture](RGBuilder &builder) {
            builder.StorageWrite(cube_texture.Mip(0));
            return [](RGComputeContext &) {};
        });
        graph.AddComputePass("ReadFace", compute_pass, [cube_texture](RGBuilder &builder) {
            builder.Sampled(cube_texture.Subresource(1, 2));
            builder.SideEffect();
            return [](RGComputeContext &) {};
        });
        graph.AddComputePass("ReadAll", compute_pass, [cube_texture](RGBuilder &builder) {
            builder.Sampled(cube_texture);
            builder.SideEffect();
            return [](RGComputeContext &) {};
        });

        Run(rhi, graph,
            {
                "ClearFace: Raster",
                "  barrier Cube[mip 1 layer 2] Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment Cube[mip 1 layer 2] slot 0: Clear (clear) / Store (read by ReadFace)",
                "WriteMip: Compute",
                "  barrier Cube[mip 0] Undefined->StorageWrite [None -> StorageWrite(Compute)]",
                "ReadFace: Compute",
                "  barrier Cube[mip 1 layer 2] ColorOutput->Read [ColorWrite -> Sampled(Compute)]",
                "ReadAll: Compute",
                "  barrier Cube[mip 0 layer 0] StorageWrite->Read [StorageWrite(Compute) -> Sampled(Compute)]",
                "  barrier Cube[mip 1 layer 0] Undefined->Read [None -> Sampled(Compute)]",
                "  barrier Cube[mip 0 layer 1] StorageWrite->Read [StorageWrite(Compute) -> Sampled(Compute)]",
                "  barrier Cube[mip 1 layer 1] Undefined->Read [None -> Sampled(Compute)]",
                "  barrier Cube[mip 0 layer 2] StorageWrite->Read [StorageWrite(Compute) -> Sampled(Compute)]",
                "  barrier Cube[mip 0 layer 3] StorageWrite->Read [StorageWrite(Compute) -> Sampled(Compute)]",
                "  barrier Cube[mip 1 layer 3] Undefined->Read [None -> Sampled(Compute)]",
                "  barrier Cube[mip 0 layer 4] StorageWrite->Read [StorageWrite(Compute) -> Sampled(Compute)]",
                "  barrier Cube[mip 1 layer 4] Undefined->Read [None -> Sampled(Compute)]",
                "  barrier Cube[mip 0 layer 5] StorageWrite->Read [StorageWrite(Compute) -> Sampled(Compute)]",
                "  barrier Cube[mip 1 layer 5] Undefined->Read [None -> Sampled(Compute)]",
            },
            "subresources");

        const RHIImageState sampled{.layout = RHIImageLayout::Read,
                                    .access = {.access = RHIAccess::Sampled, .stages = RHIShaderStageMask::Compute}};
        bool all_sampled = true;
        for (auto mip = 0u; mip < 2; mip++)
        {
            for (auto layer = 0u; layer < 6; layer++)
            {
                all_sampled = all_sampled && cube->GetState(mip, layer) == sampled;
            }
        }
        Expect(all_sampled, "the graph writes each subresource's final state through to the import");
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
                    "  barrier Out Undefined->ColorOutput [None -> ColorWrite]",
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
                "  barrier SourceColor Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment SourceColor slot 0: Clear (clear) / Store (read by DebugView)",
                "DebugView: Raster",
                "  barrier SourceColor ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                "  barrier DebugColor Undefined->ColorOutput [None -> ColorWrite]",
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
                "  barrier T1 Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment T1 slot 0: Clear (clear) / Store (read by ReadT1)",
                "ReadT1: Raster",
                "  barrier T1 ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                "  barrier T3 Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment T3 slot 0: Clear (clear) / Store (read by ReadT2)",
                "WriteT2: Raster",
                "  barrier T2 Undefined->ColorOutput [Sampled(Pixel) -> ColorWrite]",
                "  attachment T2 slot 0: Clear (clear) / Store (read by ReadT2)",
                "ReadT2: Raster",
                "  barrier T2 ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                "  barrier T3 ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                "  barrier Result Undefined->ColorOutput [None -> ColorWrite]",
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
                "  barrier Color Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment Color slot 0: Clear (clear) / DontCare (no later reader)",
                "External: External",
                "  barrier History Read->ColorOutput [Sampled(Pixel) -> ColorWrite]",
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
                "  barrier MarkerColor Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment MarkerColor slot 0: DontCare (no earlier writer) / DontCare (no later reader)",
                "Base: Raster",
                "  barrier Color Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                "  barrier Depth Undefined->DepthStencilOutput [None -> DepthWrite]",
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
                "  barrier Output Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment Output slot 0: Load (imported) / Store (imported)",
                "MarkerColor: physical 0",
                "Color: physical 0",
                "Depth: physical 1",
            },
            "load/store inference");
    }

    // a pass that fully overwrites one texture discards only that one: another it reads and writes keeps its contents
    void FullyOverwritesReadWrite(RHIContext *rhi, const RenderConfig &config)
    {
        const auto compute_pass = rhi->CreateComputePass("RenderGraphTestCompute", false);
        RGTexturePool pool(rhi);
        RenderGraph graph(pool, config);
        constexpr RGTextureDesc Desc{.format = PixelFormat::R32Float, .size_class = RGSizeClass::Scene};
        const auto sum = graph.CreateTexture("Sum", Desc);
        const auto radiance = graph.CreateTexture("Radiance", Desc);
        const auto shown = graph.CreateTexture("Shown", Rgba8Scene);
        graph.AddComputePass("Seed", compute_pass, [sum](RGBuilder &builder) {
            builder.StorageWrite(sum);
            return [](RGComputeContext &) {};
        });
        graph.AddComputePass("Accumulate", compute_pass, [sum, radiance](RGBuilder &builder) {
            builder.StorageReadWrite(sum);
            builder.StorageWrite(radiance);
            builder.FullyOverwrites();
            return [](RGComputeContext &) {};
        });
        graph.AddRasterPass("Show", [sum, radiance, shown](RGBuilder &builder) {
            builder.Sampled(sum);
            builder.Sampled(radiance);
            builder.ColorWrite(shown, 0, Vector4(0.f, 0.f, 0.f, 1.f));
            builder.SideEffect();
            return [](RGRasterContext &) {};
        });

        Run(rhi, graph,
            {
                "Seed: Compute",
                "  barrier Sum Undefined->StorageWrite [None -> StorageWrite(Compute)]",
                "Accumulate: Compute",
                "  barrier Sum StorageWrite->StorageWrite [StorageWrite(Compute) -> StorageRead|StorageWrite(Compute)]",
                "  barrier Radiance Undefined->StorageWrite [None -> StorageWrite(Compute)]",
                "Show: Raster",
                "  barrier Sum StorageWrite->Read [StorageRead|StorageWrite(Compute) -> Sampled(Pixel)]",
                "  barrier Radiance StorageWrite->Read [StorageWrite(Compute) -> Sampled(Pixel)]",
                "  barrier Shown Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment Shown slot 0: Clear (clear) / DontCare (no later reader)",
                "Sum: physical 0",
                "Radiance: physical 1",
                "Shown: physical 2",
            },
            "fully overwrites with a read-write access");
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
                first_frame ? "  barrier Shown Undefined->ColorOutput [None -> ColorWrite]"
                            : "  barrier Shown Undefined->ColorOutput [ColorWrite -> ColorWrite]",
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
    static constexpr std::array<Step, 12> Steps{
        &RenderGraphCompileTest::ClearSampleReadback,
        &RenderGraphCompileTest::DeclaredBinding,
        &RenderGraphCompileTest::BufferRoundTrip,
        &RenderGraphCompileTest::Subresources,
        &RenderGraphCompileTest::CulledBranch,
        &RenderGraphCompileTest::IntraFrameReuse,
        &RenderGraphCompileTest::ImportedSeeding,
        &RenderGraphCompileTest::LoadStore,
        &RenderGraphCompileTest::FullyOverwritesReadWrite,
        &RenderGraphCompileTest::NextFrameReuse,
        &RenderGraphCompileTest::NextFrameReuse,
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
