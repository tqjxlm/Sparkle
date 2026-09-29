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
// the screen quad's pixel shader, declared here to name its resource table
class PlaceholderQuadPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(PlaceholderQuadPixelShader, RHIShaderStage::Pixel, "shaders/screen/screen.ps.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(screenTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(screenTextureSampler, RHIShaderResourceReflection::ResourceType::Sampler)

    END_SHADER_RESOURCE_TABLE
};

namespace
{
// a screen quad whose input may be missing, drawing `placeholder` in its place
class PlaceholderQuadPass : public ScreenQuadPass
{
public:
    PlaceholderQuadPass(RHIContext *rhi, std::string name, PixelFormat output_format,
                        RHIResourceRef<RHIImage> placeholder)
        : ScreenQuadPass(rhi, std::move(name), output_format), placeholder_(std::move(placeholder))
    {
    }

protected:
    void SetupPixelShader() override
    {
        pixel_shader_ = rhi_->CreateShader<PlaceholderQuadPixelShader>();
        pipeline_state_->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
    }

    void SampleInput(RGBuilder &builder, RGTexture input, const RHISampler::SamplerAttribute &sampler) const override
    {
        using Table = PlaceholderQuadPixelShader::ResourceTable;
        builder.SampledOrPlaceholder(input, placeholder_, &Table::screenTexture, &Table::screenTextureSampler, sampler);
    }

private:
    RHIResourceRef<RHIImage> placeholder_;
};

// a screen quad that keeps the sampler its pass binds to the input
class SamplerProbePass : public ScreenQuadPass
{
public:
    using ScreenQuadPass::ScreenQuadPass;

    [[nodiscard]] const RHISampler::SamplerAttribute &GetInputSampler() const
    {
        return input_sampler_;
    }

protected:
    void SampleInput(RGBuilder &builder, RGTexture input, const RHISampler::SamplerAttribute &sampler) const override
    {
        input_sampler_ = sampler;
        ScreenQuadPass::SampleInput(builder, input, sampler);
    }

private:
    mutable RHISampler::SamplerAttribute input_sampler_;
};
} // namespace

// builds synthetic render graphs, compares their compiled plans (culling, physical images, barriers, load/store with
// reasons) against expected dump summaries, and executes each one. under synchronization validation that proves the
// planned barriers order every access, including a pooled image reused by the next graph and a buffer copied through.
// graphs read back a texture to prove the recorded passes ran and a draw binds the texture and sampler its pass
// declared. screen quads are checked for the sampler each input filter chooses.
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
        config.render_graph_full_barriers = false;

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
            for (const auto &barrier : pass.value("barriers_after", nlohmann::json::array()))
            {
                lines.push_back(std::format("  barrier after {} [{} -> {}]", GetResource(barrier),
                                            barrier.at("from").get<std::string>(),
                                            barrier.at("to").get<std::string>()));
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
                                 .width = size.x(),
                                 .height = size.y(),
                                 .usages = RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::Texture},
                                name);
    }

    // a square R8G8B8A8Unorm image
    static RHIResourceRef<RHIImage> CreateImage(RHIContext *rhi, const std::string &name, uint32_t size,
                                                RHIImage::ImageUsage usages, uint8_t mips = 1)
    {
        RHIImage::Attribute attribute;
        attribute.format = PixelFormat::R8G8B8A8Unorm;
        attribute.width = size;
        attribute.height = size;
        attribute.usages = usages;
        attribute.mip_levels = mips;
        return rhi->CreateImage(attribute, name);
    }

    void ClearSampleReadback(RHIContext *rhi, const RenderConfig &config)
    {
        auto readback = CreateReadbackBuffer(rhi, config);

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(rhi, pool, config);
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
                    "  barrier after Readback [CopyDst -> HostRead]",
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

    // copies `texture` into the imported `readback` buffer, which the host reads
    static void AddReadback(RenderGraph &graph, RGTexture texture, const RHIResourceRef<RHIBuffer> &readback)
    {
        const auto buffer = graph.Import("Readback", readback);
        graph.ReadOnHost(buffer);
        graph.AddCopyPass("Readback", [texture, buffer](RGBuilder &builder) {
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
            RenderGraph graph(rhi, pool, config);
            const auto a = graph.CreateTexture("A", Rgba8Output);
            const auto b = graph.CreateTexture("B", Rgba8Output);
            const auto buffer = graph.Import("Staging", staging);
            Expect(graph.Import("StagingAgain", staging) == buffer,
                   "importing a buffer again returns its first import");
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
                    "  barrier after Readback [CopyDst -> HostRead]",
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
        Expect(readback->GetTracked().GetTrackedAccess() == RHIResourceAccess{.access = RHIAccess::HostRead},
               "the graph writes the host read through to the buffer the host reads");

        rhi->WaitForDeviceIdle();
        const auto *pixel = static_cast<const uint8_t *>(readback->Lock());
        Expect(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255,
               "the texture copied through the buffer is read back");
        readback->UnLock();
    }

    // the round trip with full barriers, which add a memory barrier before every pass but change no plan
    void FullBarriers(RHIContext *rhi, const RenderConfig &config)
    {
        auto full_barriers = config;
        full_barriers.render_graph_full_barriers = true;
        BufferRoundTrip(rhi, full_barriers);
    }

    // a buffer the host reads becomes visible to it right after its last live pass, before the passes after it. a
    // culled pass does not count: a buffer only culled passes use gets no barrier and keeps its tracked access.
    void HostReadAfterLastPass(RHIContext *rhi, const RenderConfig &config)
    {
        const auto output = config.GetResolution().output;
        auto readback = CreateReadbackBuffer(rhi, config);
        auto unread =
            rhi->CreateBuffer({.size = output.x() * output.y() * 4u,
                               .usages = RHIBuffer::BufferUsage::TransferSrc | RHIBuffer::BufferUsage::TransferDst,
                               .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                               .is_dynamic = false},
                              "RenderGraphTestUnread");
        // as an earlier device write left it
        unread->GetTracked().SetTrackedAccess({.access = RHIAccess::CopyDst});

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(rhi, pool, config);
            const auto a = graph.CreateTexture("A", Rgba8Output);
            const auto b = graph.CreateTexture("B", Rgba8Output);
            const auto c = graph.CreateTexture("C", Rgba8Output);
            const auto unread_buffer = graph.Import("Unread", unread);
            graph.ReadOnHost(unread_buffer);
            graph.AddRasterPass("Clear", [a](RGBuilder &builder) {
                builder.ColorWrite(a, 0, Vector4(0.f, 0.f, 0.f, 1.f));
                return [](RGRasterContext &) {};
            });
            AddReadback(graph, a, readback);
            graph.AddRasterPass("Sample", [a, b](RGBuilder &builder) {
                builder.Sampled(a);
                builder.ColorWrite(b, 0, Vector4(0.f, 0.f, 0.f, 1.f));
                builder.SideEffect();
                return [](RGRasterContext &) {};
            });
            graph.AddCopyPass("Upload", [unread_buffer, c](RGBuilder &builder) {
                builder.CopySrc(unread_buffer);
                builder.CopyDst(c);
                builder.FullyOverwrites();
                return [unread_buffer, c](RGCopyContext &context) { context.CopyFromBuffer(unread_buffer, c); };
            });

            Run(rhi, graph,
                {
                    "Clear: Raster",
                    "  barrier A Undefined->ColorOutput [None -> ColorWrite]",
                    "  attachment A slot 0: Clear (clear) / Store (read by Readback)",
                    "Readback: Copy",
                    "  barrier A ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                    "  barrier after Readback [CopyDst -> HostRead]",
                    "Sample: Raster",
                    "  barrier A TransferSrc->Read [CopySrc -> Sampled(Pixel)]",
                    "  barrier B Undefined->ColorOutput [None -> ColorWrite]",
                    "  attachment B slot 0: Clear (clear) / DontCare (no later reader)",
                    "Upload: culled (unread outputs: C)",
                    "A: physical 0",
                    "B: physical 1",
                    "C: no image",
                },
                "host read after the last pass");
        }

        Expect(readback->GetTracked().GetTrackedAccess() == RHIResourceAccess{.access = RHIAccess::HostRead},
               "the graph writes the host read through to the buffer the host reads");
        Expect(unread->GetTracked().GetTrackedAccess() == RHIResourceAccess{.access = RHIAccess::CopyDst},
               "a buffer the host reads that no live pass uses keeps its tracked access");
    }

    // a screen quad pipeline created from an attachment signature, with nothing bound to sample, draws a cleared
    // texture, which reaches the readback only when the draw binds the texture and sampler its pass declared
    void DeclaredBinding(RHIContext *rhi, const RenderConfig &config)
    {
        const auto quad = PipelinePass::Create<ScreenQuadPass>(config, rhi, "Quad", Rgba8Output.format);
        auto readback = CreateReadbackBuffer(rhi, config);

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(rhi, pool, config);
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
                    "  barrier after Readback [CopyDst -> HostRead]",
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

    // a quad whose input is missing binds and draws a placeholder image outside the graph, which the plan never names
    void Placeholder(RHIContext *rhi, const RenderConfig &config)
    {
        auto placeholder = CreateImage(rhi, "RenderGraphTestPlaceholder", 4,
                                       RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::TransferDst);
        std::array<uint8_t, 4u * 4u * 4u> magenta{};
        for (auto texel = 0u; texel < magenta.size(); texel += 4)
        {
            magenta[texel] = 255;
            magenta[texel + 2] = 255;
            magenta[texel + 3] = 255;
        }
        rhi->BeginCommandBuffer();
        placeholder->Upload(magenta.data());
        rhi->SubmitCommandBuffer();

        const auto quad =
            PipelinePass::Create<PlaceholderQuadPass>(config, rhi, "Quad", Rgba8Output.format, placeholder);
        auto readback = CreateReadbackBuffer(rhi, config);

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(rhi, pool, config);
            const auto b = graph.CreateTexture("B", Rgba8Output);
            quad->AddTo(graph, {}, b);
            AddReadback(graph, b, readback);

            Run(rhi, graph,
                {
                    "Quad: Raster",
                    "  barrier B Undefined->ColorOutput [None -> ColorWrite]",
                    "  attachment B slot 0: DontCare (fully overwritten) / Store (read by Readback)",
                    "Readback: Copy",
                    "  barrier B ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                    "  barrier after Readback [CopyDst -> HostRead]",
                    "B: physical 0",
                },
                "placeholder");
        }

        rhi->WaitForDeviceIdle();
        const auto *pixel = static_cast<const uint8_t *>(readback->Lock());
        Expect(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255,
               "the quad draws the placeholder of its missing input");
        readback->UnLock();
    }

    // a raster pass declaring native access records through the raw command context, inside the rendering the graph
    // began over its attachments
    void NativeRecording(RHIContext *rhi, const RenderConfig &config)
    {
        auto readback = CreateReadbackBuffer(rhi, config);
        bool inside_rendering = false;

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(rhi, pool, config);
            const auto a = graph.CreateTexture("A", Rgba8Output);
            graph.AddRasterPass("Native", [a, &inside_rendering](RGBuilder &builder) {
                builder.ColorWrite(a, 0, Vector4(1.f, 1.f, 0.f, 1.f));
                builder.NativeAccess();
                return [a, &inside_rendering](RGRasterContext &context) {
                    const auto &attachment = context.GetNativeContext().GetRenderingInfo().color_attachments[0];
                    inside_rendering =
                        attachment.image == context.GetImage(a) && attachment.load_op == RHILoadOp::Clear;
                };
            });
            AddReadback(graph, a, readback);

            Run(rhi, graph,
                {
                    "Native: Raster",
                    "  barrier A Undefined->ColorOutput [None -> ColorWrite]",
                    "  attachment A slot 0: Clear (clear) / Store (read by Readback)",
                    "Readback: Copy",
                    "  barrier A ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                    "  barrier after Readback [CopyDst -> HostRead]",
                    "A: physical 0",
                },
                "native recording");
        }
        Expect(inside_rendering, "the native context records inside the rendering over the pass's attachment");

        rhi->WaitForDeviceIdle();
        const auto *pixel = static_cast<const uint8_t *>(readback->Lock());
        Expect(pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255,
               "the native pass's attachment is cleared");
        readback->UnLock();
    }

    // a build waits for earlier builds (the BLAS it reads, the scratch memory it reuses), and a ray query for the
    // build. acceleration structures exist only with hardware ray tracing.
    void AccelerationStructureBuilds(RHIContext *rhi, const RenderConfig &config)
    {
        if (!rhi->SupportsHardwareRayTracing())
        {
            Log(Info, "{}: no hardware ray tracing, skipping acceleration structure builds", GetName());
            return;
        }

        auto tlas = rhi->CreateTLAS("RenderGraphTestTLAS");
        const auto compute_pass = rhi->CreateComputePass("RenderGraphTestCompute", false);
        RGTexturePool pool(rhi);
        RenderGraph graph(rhi, pool, config);
        const auto tlas_import = graph.Import("TLAS", tlas);
        for (const auto *name : {"Build", "Refit"})
        {
            graph.AddCopyPass(name, [tlas_import](RGBuilder &builder) {
                builder.AccelerationStructureBuild(tlas_import);
                return [](RGCopyContext &) {};
            });
        }
        graph.AddComputePass("Trace", compute_pass, [tlas_import](RGBuilder &builder) {
            builder.AccelerationStructureRead(tlas_import);
            builder.SideEffect();
            return [](RGComputeContext &) {};
        });

        Run(rhi, graph,
            {
                "Build: Copy",
                "  barrier TLAS [AccelerationStructureBuild -> AccelerationStructureBuild]",
                "Refit: Copy",
                "  barrier TLAS [AccelerationStructureBuild -> AccelerationStructureBuild]",
                "Trace: Compute",
                "  barrier TLAS [AccelerationStructureBuild -> AccelerationStructureRead(Compute)]",
            },
            "acceleration structure builds");
        Expect(tlas->GetTracked().GetTrackedAccess() ==
                   RHIResourceAccess{.access = RHIAccess::AccelerationStructureRead,
                                     .stages = RHIShaderStageMask::Compute},
               "the graph writes the ray query through to the acceleration structure");
    }

    // a screen quad filters its input bilinearly, edge-clamped, only when its filter resamples the input and the
    // device filters the input's format linearly. Bilinear resamples an input of another size than the 64x32 output,
    // NearestAtIntegerScale one the output is not an integer multiple of in both axes.
    void ScreenInputFilter(RHIContext *rhi, const RenderConfig &config)
    {
        using Filter = ScreenQuadPass::InputFilter;
        const auto bilinear_quad =
            PipelinePass::Create<SamplerProbePass>(config, rhi, "Upsample", Rgba8Output.format, Filter::Bilinear);
        const auto integer_scale_quad = PipelinePass::Create<SamplerProbePass>(
            config, rhi, "Present", Rgba8Output.format, Filter::NearestAtIntegerScale);

        constexpr RGTextureDesc RgbaFloatScene{.format = PixelFormat::RGBAFloat, .size_class = RGSizeClass::Scene};
        constexpr auto Absolute = [](PixelFormat format, uint32_t width, uint32_t height) {
            return RGTextureDesc{
                .format = format, .size_class = RGSizeClass::Absolute, .width = width, .height = height};
        };

        struct Case
        {
            Filter filter;
            RGTextureDesc input_desc;
            bool resamples;
        };

        for (const auto &[filter, input_desc, resamples] : {
                 Case{Filter::Bilinear, Rgba8Scene, true},
                 Case{Filter::Bilinear, Rgba8Output, false},
                 Case{Filter::Bilinear, RgbaFloatScene, true},
                 Case{Filter::NearestAtIntegerScale, Rgba8Scene, false},
                 Case{Filter::NearestAtIntegerScale, Rgba8Output, false},
                 Case{Filter::NearestAtIntegerScale, Absolute(PixelFormat::R8G8B8A8Unorm, 48, 24), true},
                 Case{Filter::NearestAtIntegerScale, Absolute(PixelFormat::R8G8B8A8Unorm, 64, 24), true},
                 Case{Filter::NearestAtIntegerScale, Absolute(PixelFormat::RGBAFloat, 48, 24), true},
             })
        {
            const auto &quad = filter == Filter::Bilinear ? bilinear_quad : integer_scale_quad;
            RGTexturePool pool(rhi);
            RenderGraph graph(rhi, pool, config);
            const auto input = graph.CreateTexture("Input", input_desc);
            quad->AddTo(graph, input, graph.CreateTexture("Output", Rgba8Output));

            const bool bilinear = resamples && rhi->SupportsLinearFiltering(input_desc.format);
            const auto method = bilinear ? RHISampler::FilteringMethod::Linear : RHISampler::FilteringMethod::Nearest;
            const auto &sampler = quad->GetInputSampler();
            const auto size = graph.GetSize(input);
            Expect(sampler.address_mode == RHISampler::SamplerAddressMode::ClampToEdge &&
                       sampler.filtering_method_min == method && sampler.filtering_method_mag == method,
                   std::format("{} samples a {}x{} {} input {}", Enum2Str(filter), size.x(), size.y(),
                               Enum2Str(input_desc.format), bilinear ? "bilinearly" : "nearest"));
        }
    }

    // a face of one mip cleared, another mip written and the face sampled, then the whole cube sampled: each access
    // plans barriers for its own subresources, the clear is stored for the reader of its face rather than dropped for
    // the writer of the other mip, and the face already sampled needs no second barrier
    void Subresources(RHIContext *rhi, const RenderConfig &config)
    {
        auto cube = rhi->CreateImage({.format = PixelFormat::R8G8B8A8Unorm,
                                      .width = 8,
                                      .height = 8,
                                      .usages = RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::UAV |
                                                RHIImage::ImageUsage::Texture,
                                      .mip_levels = 2,
                                      .type = RHIImage::ImageType::Image2DCube},
                                     "RenderGraphTestCube");
        const auto compute_pass = rhi->CreateComputePass("RenderGraphTestCompute", false);

        RGTexturePool pool(rhi);
        RenderGraph graph(rhi, pool, config);
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

    // a downsample samples mip 0 and writes mips 1 and 2, then the whole image is sampled: the mips left in one state
    // move with one barrier over their run, and mip 0, already sampled, needs none
    void MipRun(RHIContext *rhi, const RenderConfig &config)
    {
        auto mips =
            CreateImage(rhi, "RenderGraphTestMips", 8, RHIImage::ImageUsage::UAV | RHIImage::ImageUsage::Texture, 3);
        const auto compute_pass = rhi->CreateComputePass("RenderGraphTestCompute", false);

        RGTexturePool pool(rhi);
        RenderGraph graph(rhi, pool, config);
        const auto mips_texture = graph.Import("Mips", mips);
        graph.AddComputePass("WriteBase", compute_pass, [mips_texture](RGBuilder &builder) {
            builder.StorageWrite(mips_texture.Mip(0));
            return [](RGComputeContext &) {};
        });
        graph.AddComputePass("Downsample", compute_pass, [mips_texture](RGBuilder &builder) {
            builder.Sampled(mips_texture.Mip(0));
            builder.StorageWrite(mips_texture.Mip(1));
            builder.StorageWrite(mips_texture.Mip(2));
            return [](RGComputeContext &) {};
        });
        graph.AddComputePass("ReadAll", compute_pass, [mips_texture](RGBuilder &builder) {
            builder.Sampled(mips_texture);
            builder.SideEffect();
            return [](RGComputeContext &) {};
        });

        Run(rhi, graph,
            {
                "WriteBase: Compute",
                "  barrier Mips[mip 0] Undefined->StorageWrite [None -> StorageWrite(Compute)]",
                "Downsample: Compute",
                "  barrier Mips[mip 0] StorageWrite->Read [StorageWrite(Compute) -> Sampled(Compute)]",
                "  barrier Mips[mip 1] Undefined->StorageWrite [None -> StorageWrite(Compute)]",
                "  barrier Mips[mip 2] Undefined->StorageWrite [None -> StorageWrite(Compute)]",
                "ReadAll: Compute",
                "  barrier Mips[mips 1-2] StorageWrite->Read [StorageWrite(Compute) -> Sampled(Compute)]",
            },
            "mip run");
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
            RenderGraph graph(rhi, pool, config);
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
        RenderGraph graph(rhi, pool, no_cull);
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
        RenderGraph graph(rhi, pool, config);
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
        RenderGraph graph(rhi, pool, config);
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
        RenderGraph graph(rhi, pool, config);
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
        RenderGraph graph(rhi, pool, config);
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
        RenderGraph graph(rhi, pool, config);
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

    void ReleaseUnused(RHIContext *rhi, const RenderConfig &config)
    {
        auto &pool = *resources_.reuse_pool;
        const auto run_empty_graph = [rhi, &pool, &config] {
            RenderGraph graph(rhi, pool, config);
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
    static constexpr std::array<Step, 19> Steps{
        &RenderGraphCompileTest::ClearSampleReadback,
        &RenderGraphCompileTest::DeclaredBinding,
        &RenderGraphCompileTest::Placeholder,
        &RenderGraphCompileTest::NativeRecording,
        &RenderGraphCompileTest::BufferRoundTrip,
        &RenderGraphCompileTest::FullBarriers,
        &RenderGraphCompileTest::HostReadAfterLastPass,
        &RenderGraphCompileTest::AccelerationStructureBuilds,
        &RenderGraphCompileTest::Subresources,
        &RenderGraphCompileTest::MipRun,
        &RenderGraphCompileTest::CulledBranch,
        &RenderGraphCompileTest::IntraFrameReuse,
        &RenderGraphCompileTest::ImportedSeeding,
        &RenderGraphCompileTest::LoadStore,
        &RenderGraphCompileTest::FullyOverwritesReadWrite,
        &RenderGraphCompileTest::NextFrameReuse,
        &RenderGraphCompileTest::NextFrameReuse,
        &RenderGraphCompileTest::ReleaseUnused,
        &RenderGraphCompileTest::ScreenInputFilter,
    };

    size_t step_ = 0;
    std::atomic<bool> task_pending_{false};
    std::atomic<bool> failed_{false};

    // only accessed from the render thread
    Resources resources_;
};

static TestCaseRegistrar<RenderGraphCompileTest> render_graph_compile_test_registrar("render_graph_compile");
} // namespace sparkle
