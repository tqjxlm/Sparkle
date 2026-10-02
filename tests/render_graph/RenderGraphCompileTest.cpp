#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "renderer/graph/RGTexturePool.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/pass/ToneMappingPass.h"
#include "rhi/RHI.h"

#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_flags.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <functional>
#include <memory>
#include <tuple>

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
// a screen quad whose input is missing, drawing `placeholder` in its place
class PlaceholderQuadPass : public ScreenQuadPass
{
public:
    PlaceholderQuadPass(RHIContext *rhi, std::string name, PixelFormat output_format,
                        RHIResourceRef<RHIImage> placeholder)
        : ScreenQuadPass(rhi, std::move(name), output_format, InputFilter::Nearest),
          placeholder_(std::move(placeholder))
    {
    }

    void AddTo(RenderGraph &graph, RGTexture output) const
    {
        graph.AddRasterPass(name_, [this, output](RGBuilder &builder) {
            using Table = PlaceholderQuadPixelShader::ResourceTable;
            builder.SampledOrPlaceholder(RGTexture{}, placeholder_, &Table::screenTexture, &Table::screenTextureSampler,
                                         NearestSampler);
            builder.ColorWrite(output, 0);
            builder.FullyOverwrites();
            return [this](RGRasterContext &context) { context.DrawMesh(pipeline_state_, draw_args_); };
        });
    }

protected:
    void SetupPixelShader() override
    {
        pixel_shader_ = rhi_->CreateShader<PlaceholderQuadPixelShader>();
        pipeline_state_->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
    }

private:
    RHIResourceRef<RHIImage> placeholder_;
};

// a screen quad whose pipeline keeps the default depth state, which tests and writes depth
class DepthTestingQuadPass : public ScreenQuadPass
{
public:
    using ScreenQuadPass::AddTo;
    using ScreenQuadPass::ScreenQuadPass;

    // draws `input` over `output` where the quad passes the depth test against `depth`
    void AddTo(RenderGraph &graph, RGTexture input, RGTexture output, RGTexture depth) const
    {
        graph.AddRasterPass(name_, [this, input, output, depth](RGBuilder &builder) {
            SampleInput(builder, input, NearestSampler);
            builder.ColorWrite(output, 0);
            builder.DepthTest(depth);
            return [this](RGRasterContext &context) { context.DrawMesh(pipeline_state_, draw_args_); };
        });
    }

protected:
    void SetupPixelShader() override
    {
        ScreenQuadPass::SetupPixelShader();
        pipeline_state_->SetDepthState({});
    }
};

// tone mapping at an exposure the test sets, which reads its input pixel-locally at slot 1
class ExposedToneMappingPass : public ToneMappingPass
{
public:
    using ToneMappingPass::ToneMappingPass;

    void SetExposure(float exposure) const
    {
        ps_ub_->Upload(rhi_, &exposure);
    }
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

// builds synthetic render graphs, compares their compiled plans (culling, physical images, physical passes and why
// they break, barriers, load/store with reasons) against expected dump summaries, and executes each one. under
// synchronization validation that proves the planned barriers order every access, including a pooled image reused by
// the next graph and a buffer copied through. graphs read back a texture to prove the recorded passes ran and a draw
// binds the texture and sampler its pass declared. screen quads are checked for the sampler each input filter chooses.
class RenderGraphCompileTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        if (HasFailed())
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
        config.render_graph_merge = true;
        config.render_graph_full_barriers = false;
        config.render_graph_tile_budget = 0;
        config.render_graph_tile_budget_split = false;

        task_pending_.store(true, std::memory_order_release);
        const bool last = step_ + 1 == Steps.size();
        TaskManager::RunInRenderThread([this, rhi = app.GetRHI(), config, step = Steps[step_], last] {
            (this->*step)(rhi, config);
            if (last || HasFailed())
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
    static constexpr RGTextureDesc DepthOutput{.format = PixelFormat::D32, .size_class = RGSizeClass::Output};

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

    // the members of a physical pass joined by '+'
    static std::string JoinMembers(const nlohmann::json &dump, const nlohmann::json &physical)
    {
        std::string joined;
        for (const auto &member : physical.at("members"))
        {
            joined += (joined.empty() ? "" : "+") +
                      dump.at("passes").at(member.get<unsigned>()).at("name").get<std::string>();
        }
        return joined;
    }

    // each pass with its barriers, then each physical pass's attachments after its last member, named when it merges
    // passes
    static std::vector<std::string> Summarize(const nlohmann::json &dump)
    {
        std::vector<std::string> lines;
        const auto &passes = dump.at("passes");
        for (auto index = 0u; index < passes.size(); index++)
        {
            const auto &pass = passes.at(index);
            const auto name = pass.at("name").get<std::string>();
            if (pass.at("culled").get<bool>())
            {
                lines.push_back(std::format("{}: culled ({})", name, pass.at("cull_reason").get<std::string>()));
                continue;
            }

            const auto step = pass.at("step").get<unsigned>();
            lines.push_back(step == 0 ? std::format("{}: {}", name, pass.at("kind").get<std::string>())
                                      : std::format("{}: {}, step {}", name, pass.at("kind").get<std::string>(), step));
            for (const auto &barrier : pass.at("barriers"))
            {
                // memory barriers have no layouts
                const auto layouts = barrier.contains("from_layout")
                                         ? std::format(" {}->{}", barrier.at("from_layout").get<std::string>(),
                                                       barrier.at("to_layout").get<std::string>())
                                         : std::string();
                lines.push_back(std::format("  barrier{} {}{} [{} -> {}]",
                                            barrier.value("in_rendering", false) ? " in rendering" : "",
                                            GetResource(barrier), layouts, barrier.at("from").get<std::string>(),
                                            barrier.at("to").get<std::string>()));
            }
            for (const auto &barrier : pass.value("barriers_after", nlohmann::json::array()))
            {
                lines.push_back(std::format("  barrier after {} [{} -> {}]", GetResource(barrier),
                                            barrier.at("from").get<std::string>(),
                                            barrier.at("to").get<std::string>()));
            }

            const auto &physical = dump.at("physical_passes").at(pass.at("physical_pass").get<unsigned>());
            const auto &members = physical.at("members");
            if (members.back().get<unsigned>() != index)
            {
                continue;
            }
            if (members.size() > 1)
            {
                lines.push_back("  physical " + JoinMembers(dump, physical));
            }
            for (const auto &attachment : physical.at("attachments"))
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
                const auto name = resource.at("name").get<std::string>();
                lines.push_back(resource.contains("physical")
                                    ? std::format("{}: physical {}{}", name, resource.at("physical").get<unsigned>(),
                                                  resource.at("memoryless").get<bool>() ? ", memoryless" : "")
                                    : std::format("{}: no image", name));
            }
        }
        return lines;
    }

    // why each physical pass ends before the next: "<its members> | <the next one's members>: <reason>(<resource>)"
    static std::vector<std::string> Breaks(const nlohmann::json &dump)
    {
        std::vector<std::string> lines;
        const auto &physical_passes = dump.at("physical_passes");
        for (auto index = 0u; index + 1 < physical_passes.size(); index++)
        {
            const auto &physical = physical_passes.at(index);
            const auto resource = physical.contains("break_resource")
                                      ? std::format("({})", physical.at("break_resource").get<std::string>())
                                      : std::string();
            lines.push_back(std::format("{} | {}: {}{}", JoinMembers(dump, physical),
                                        JoinMembers(dump, physical_passes.at(index + 1)),
                                        physical.at("break_reason").get<std::string>(), resource));
        }
        return lines;
    }

    // each pixel-local read request: "<pass> <resource>: PixelLocalRead", or "lowered <reason>" with the reason it was
    // lowered to Sampled
    static std::vector<std::string> PixelLocalReads(const nlohmann::json &dump)
    {
        std::vector<std::string> lines;
        for (const auto &pass : dump.at("passes"))
        {
            for (const auto &access : pass.at("accesses"))
            {
                if (access.contains("pixel_local_slot"))
                {
                    lines.push_back(std::format("{} {}: {}", pass.at("name").get<std::string>(), GetResource(access),
                                                access.contains("lowered_reason")
                                                    ? "lowered " + access.at("lowered_reason").get<std::string>()
                                                    : access.at("access").get<std::string>()));
                }
            }
        }
        return lines;
    }

    // expects `lines` from the compiled graph's `what`
    void ExpectLines(const std::vector<std::string> &lines, const std::vector<std::string> &expected,
                     const std::string &what, const nlohmann::json &dump)
    {
        Expect(lines == expected, what);
        if (lines != expected)
        {
            Log(Error, "expected:");
            for (const auto &line : expected)
            {
                Log(Error, "{}", line);
            }
            Log(Error, "got (full dump {}):", dump.dump());
            for (const auto &line : lines)
            {
                Log(Error, "{}", line);
            }
        }
    }

    // compiles, checks the plan and the images backing it, and records the graph into its own command buffer
    void Run(RHIContext *rhi, RenderGraph &graph, const std::vector<std::string> &expected, const std::string &what)
    {
        graph.Compile();

        const auto dump = graph.Dump();
        ExpectLines(Summarize(dump), expected, std::format("{} plan", what), dump);
        ExpectBackings(rhi, dump, what);

        Record(rhi, graph);
    }

    // a memoryless transient has a memoryless image where the device has memoryless storage for its format and usages,
    // every other transient a pooled one
    void ExpectBackings(RHIContext *rhi, const nlohmann::json &dump, const std::string &what)
    {
        for (const auto &resource : dump.at("resources"))
        {
            if (resource.contains("physical"))
            {
                const auto format = magic_enum::enum_cast<PixelFormat>(resource.at("format").get<std::string>());
                const auto usages =
                    magic_enum::enum_flags_cast<RHIImage::ImageUsage>(resource.at("usage").get<std::string>());
                const bool memoryless = resource.at("memoryless").get<bool>() && format && usages &&
                                        rhi->SupportsMemorylessImage(*format, *usages);
                Expect(resource.at("backing") == (memoryless ? "memoryless" : "pooled"),
                       std::format("{}: {} has a {} image", what, resource.at("name").get<std::string>(),
                                   memoryless ? "memoryless" : "pooled"));
            }
        }
    }

    static void Record(RHIContext *rhi, RenderGraph &graph)
    {
        graph.Execute(rhi->BeginCommandBuffer());
        rhi->SubmitCommandBuffer();
    }

    void ExpectBreaks(const RenderGraph &graph, const std::vector<std::string> &expected, const std::string &what)
    {
        const auto dump = graph.Dump();
        ExpectLines(Breaks(dump), expected, std::format("{} breaks", what), dump);
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
                                                RHIImage::ImageUsage usages, uint8_t mips = 1,
                                                RHIImage::ImageType type = RHIImage::ImageType::Image2D)
    {
        RHIImage::Attribute attribute;
        attribute.format = PixelFormat::R8G8B8A8Unorm;
        attribute.width = size;
        attribute.height = size;
        attribute.usages = usages;
        attribute.mip_levels = mips;
        attribute.type = type;
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

    static void AddClear(RenderGraph &graph, std::string name, RGTextureRange texture, uint8_t slot,
                         const Vector4 &color)
    {
        graph.AddRasterPass(std::move(name), [texture, slot, color](RGBuilder &builder) {
            builder.ColorWrite(texture, slot, color);
            return [](RGRasterContext &) {};
        });
    }

    // expects the first texel of an R8G8B8A8 readback
    void ExpectTexel(RHIContext *rhi, const RHIResourceRef<RHIBuffer> &readback, const std::array<uint8_t, 4> &texel,
                     const std::string &what)
    {
        rhi->WaitForDeviceIdle();
        const auto *pixel = static_cast<const uint8_t *>(readback->Lock());
        Expect(std::equal(texel.begin(), texel.end(), pixel), what);
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

    // full barriers add a memory barrier before every live pass, the first one included, and change no plan
    void FullBarriers(RHIContext *rhi, const RenderConfig &config)
    {
        auto full_barriers = config;
        full_barriers.render_graph_full_barriers = true;
        BufferRoundTrip(rhi, full_barriers);

        auto readback = CreateReadbackBuffer(rhi, config);
        RGTexturePool pool(rhi);
        RenderGraph graph(rhi, pool, full_barriers);
        const auto a = graph.CreateTexture("A", Rgba8Output);
        const auto unread = graph.CreateTexture("Unread", Rgba8Output);
        graph.AddRasterPass("Clear", [a](RGBuilder &builder) {
            builder.ColorWrite(a, 0, Vector4(1.f, 0.f, 0.f, 1.f));
            return [](RGRasterContext &) {};
        });
        graph.AddRasterPass("Unread", [unread](RGBuilder &builder) {
            builder.ColorWrite(unread, 0, Vector4(1.f, 0.f, 0.f, 1.f));
            return [](RGRasterContext &) {};
        });
        AddReadback(graph, a, readback);
        Run(rhi, graph,
            {
                "Clear: Raster",
                "  barrier A Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment A slot 0: Clear (clear) / Store (read by Readback)",
                "Unread: culled (unread outputs: Unread)",
                "Readback: Copy",
                "  barrier A ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                "  barrier after Readback [CopyDst -> HostRead]",
                "A: physical 0",
                "Unread: no image",
            },
            "full barriers");

        const auto physical_passes = graph.Dump().at("physical_passes");
        Expect(physical_passes.size() == 2 && std::ranges::all_of(physical_passes,
                                                                  [](const nlohmann::json &physical) {
                                                                      return physical.value("full_barrier", false);
                                                                  }),
               "every physical pass records a full barrier");
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
                    "B: physical 1, memoryless",
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
        const auto quad = PipelinePass::Create<ScreenQuadPass>(config, rhi, "Quad", Rgba8Output.format,
                                                               ScreenQuadPass::InputFilter::Nearest);
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
        placeholder->Upload(rhi->BeginCommandBuffer(), magenta.data());
        rhi->SubmitCommandBuffer();

        const auto quad =
            PipelinePass::Create<PlaceholderQuadPass>(config, rhi, "Quad", Rgba8Output.format, placeholder);
        auto readback = CreateReadbackBuffer(rhi, config);

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(rhi, pool, config);
            const auto b = graph.CreateTexture("B", Rgba8Output);
            quad->AddTo(graph, b);
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

    // a raster pass declaring native access merges like any other and records through the raw command context, inside
    // the rendering of its physical pass, whose signature masks the slot it does not attach
    void NativeRecording(RHIContext *rhi, const RenderConfig &config)
    {
        auto readback = CreateReadbackBuffer(rhi, config);
        bool inside_rendering = false;

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(rhi, pool, config);
            const auto a = graph.CreateTexture("A", Rgba8Output);
            const auto b = graph.CreateTexture("B", Rgba8Output);
            graph.AddRasterPass("Base", [a, b](RGBuilder &builder) {
                builder.ColorWrite(a, 0, Vector4(1.f, 1.f, 0.f, 1.f));
                builder.ColorWrite(b, 1, Vector4(0.f, 0.f, 1.f, 1.f));
                return [](RGRasterContext &) {};
            });
            graph.AddRasterPass("Native", [a, &inside_rendering](RGBuilder &builder) {
                builder.ColorWrite(a, 0);
                builder.NativeAccess();
                return [a, &inside_rendering](RGRasterContext &context) {
                    const auto &native_context = context.GetNativeContext();
                    const auto &attachments = native_context.GetRenderingInfo().color_attachments;
                    inside_rendering = attachments[0].image == context.GetImage(a) &&
                                       attachments[0].load_op == RHILoadOp::Clear && attachments[1].image != nullptr &&
                                       native_context.GetAttachmentSignature().unwritten_color_slots == 0b10;
                };
            });
            AddReadback(graph, a, readback);

            Run(rhi, graph,
                {
                    "Base: Raster",
                    "  barrier A Undefined->ColorOutput [None -> ColorWrite]",
                    "  barrier B Undefined->ColorOutput [None -> ColorWrite]",
                    "Native: Raster, step 1",
                    "  physical Base+Native",
                    "  attachment A slot 0: Clear (clear) / Store (read by Readback)",
                    "  attachment B slot 1: Clear (clear) / DontCare (no later reader)",
                    "Readback: Copy",
                    "  barrier A ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                    "  barrier after Readback [CopyDst -> HostRead]",
                    "A: physical 0",
                    "B: physical 1, memoryless",
                },
                "native recording");
        }
        Expect(inside_rendering, "the native context records inside the rendering of its physical pass");

        rhi->WaitForDeviceIdle();
        const auto *pixel = static_cast<const uint8_t *>(readback->Lock());
        Expect(pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255,
               "the physical pass's attachment is cleared");
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

    // a downsample of a cube samples mip 0 and writes mips 1 and 2 of every face, then the whole cube is sampled: the
    // mips left in one state move with one barrier over their run across the faces, and mip 0, already sampled, needs
    // none
    void MipRun(RHIContext *rhi, const RenderConfig &config)
    {
        auto mips =
            CreateImage(rhi, "RenderGraphTestMips", 8, RHIImage::ImageUsage::UAV | RHIImage::ImageUsage::Texture, 3,
                        RHIImage::ImageType::Image2DCube);
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
                "DebugColor: physical 1, memoryless",
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
                    ->Transition(context.GetCommandContext(), {.target_layout = RHIImageLayout::ColorOutput,
                                                               .after_stage = RHIPipelineStage::ColorOutput,
                                                               .before_stage = RHIPipelineStage::ColorOutput});
            };
        });
        graph.AddRasterPass("WriteHistory", [history_texture](RGBuilder &builder) {
            builder.ColorWrite(history_texture, 0);
            return [](RGRasterContext &) {};
        });

        // the tracked state the graph starts from: sampled by pixel shaders
        history->Transition(rhi->BeginCommandBuffer(), {.target_layout = RHIImageLayout::Read,
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
                "Color: physical 0, memoryless",
            },
            "imported seeding");
        Expect(history->GetState(0, 0) ==
                   RHIImageState{.layout = RHIImageLayout::ColorOutput, .access = {.access = RHIAccess::ColorWrite}},
               "the graph writes the final state through to the import");
    }

    // per pass without merging; merged, Base, Overlay and Fullscreen share one rendering, whose load ops come from the
    // first member attaching each subresource and store ops from the passes after it, with no barrier between members
    void LoadStore(RHIContext *rhi, const RenderConfig &config)
    {
        const auto build = [rhi, &config](RenderGraph &graph) {
            const auto marker = graph.CreateTexture("MarkerColor", Rgba8Scene);
            const auto color = graph.CreateTexture("Color", Rgba8Scene);
            const auto depth =
                graph.CreateTexture("Depth", {.format = PixelFormat::D32, .size_class = RGSizeClass::Scene});
            const auto output_texture =
                graph.Import("Output", CreateImportImage(rhi, config.GetResolution().scene, "RenderGraphTestOutput"));
            graph.AddRasterPass("MarkerClear", [marker](RGBuilder &builder) {
                builder.ColorWrite(marker, 0, Vector4(0.f, 0.f, 0.f, 1.f));
                return [](RGRasterContext &) {};
            });
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
        };

        {
            auto no_merge = config;
            no_merge.render_graph_merge = false;
            RGTexturePool pool(rhi);
            RenderGraph graph(rhi, pool, no_merge);
            build(graph);
            Run(rhi, graph,
                {
                    "MarkerClear: culled (unread outputs: MarkerColor)",
                    "Marker: Raster",
                    "  barrier MarkerColor Undefined->ColorOutput [None -> ColorWrite]",
                    "  attachment MarkerColor slot 0: DontCare (no earlier writer) / DontCare (no later reader)",
                    "Base: Raster",
                    "  barrier Color Undefined->ColorOutput [None -> ColorWrite]",
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
                    "MarkerColor: physical 0, memoryless",
                    "Color: physical 1",
                    "Depth: physical 2",
                },
                "load/store inference");
            ExpectBreaks(graph,
                         {
                             "Marker | Base: SlotConflict(Color)",
                             "Base | Overlay: Disabled",
                             "Overlay | Fullscreen: Disabled",
                             "Fullscreen | Compose: SlotConflict(Output)",
                         },
                         "load/store inference");
        }

        RGTexturePool pool(rhi);
        RenderGraph graph(rhi, pool, config);
        build(graph);
        Run(rhi, graph,
            {
                "MarkerClear: culled (unread outputs: MarkerColor)",
                "Marker: Raster",
                "  barrier MarkerColor Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment MarkerColor slot 0: DontCare (no earlier writer) / DontCare (no later reader)",
                "Base: Raster",
                "  barrier Color Undefined->ColorOutput [None -> ColorWrite]",
                "  barrier Depth Undefined->DepthStencilOutput [None -> DepthWrite]",
                "Overlay: Raster",
                "Fullscreen: Raster, step 1",
                "  physical Base+Overlay+Fullscreen",
                "  attachment Color slot 0: Clear (clear) / Store (read by Compose)",
                "  attachment Depth slot depth: Clear (clear) / DontCare (no later reader)",
                "Compose: Raster",
                "  barrier Color ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                "  barrier Output Undefined->ColorOutput [None -> ColorWrite]",
                "  attachment Output slot 0: Load (imported) / Store (imported)",
                "MarkerColor: physical 0, memoryless",
                "Color: physical 1",
                "Depth: physical 2, memoryless",
            },
            "merged load/store");
        ExpectBreaks(graph,
                     {
                         "Marker | Base+Overlay+Fullscreen: SlotConflict(Color)",
                         "Base+Overlay+Fullscreen | Compose: SlotConflict(Output)",
                     },
                     "merged load/store");
    }

    // one graph breaking its physical passes for every reason in turn, with and without merging, which changes only
    // whether a depth-less clear joins the depth pass after it. every pass's result reaches the readbacks.
    void BreakReasons(RHIContext *rhi, const RenderConfig &config)
    {
        auto x_readback = CreateReadbackBuffer(rhi, config);
        auto y_readback = CreateReadbackBuffer(rhi, config);
        const auto build = [&x_readback, &y_readback](RenderGraph &graph) {
            const auto x = graph.CreateTexture("X", Rgba8Output);
            const auto y = graph.CreateTexture("Y", Rgba8Output);
            const auto d1 = graph.CreateTexture("D1", DepthOutput);
            const auto d2 = graph.CreateTexture("D2", DepthOutput);
            AddClear(graph, "ClearX", x, 0, Vector4(1.f, 0.f, 0.f, 1.f));
            graph.AddRasterPass("SampleX", [x, y](RGBuilder &builder) {
                builder.Sampled(x);
                builder.ColorWrite(y, 1, Vector4(0.f, 0.f, 1.f, 1.f));
                return [](RGRasterContext &) {};
            });
            graph.AddRasterPass("AttachX", [x](RGBuilder &builder) {
                builder.ColorWrite(x, 0);
                builder.SideEffect();
                return [](RGRasterContext &) {};
            });
            AddClear(graph, "ClearXAgain", x, 0, Vector4(0.f, 1.f, 0.f, 1.f));
            for (const auto &[name, depth] : {std::pair{"DepthA", d1}, std::pair{"DepthB", d2}})
            {
                graph.AddRasterPass(name, [x, depth](RGBuilder &builder) {
                    builder.ColorWrite(x, 0);
                    builder.DepthWrite(depth, 1.f);
                    return [](RGRasterContext &) {};
                });
            }
            graph.AddRasterPass("SlotY", [y](RGBuilder &builder) {
                builder.ColorWrite(y, 0);
                return [](RGRasterContext &) {};
            });
            graph.AddExternalPass("External", [y](RGBuilder &builder) {
                builder.ColorWrite(y, 0);
                return [](RGExternalContext &) {};
            });
            AddReadback(graph, x, x_readback);
            AddReadback(graph, y, y_readback);
        };

        for (const bool merge : {true, false})
        {
            auto merge_config = config;
            merge_config.render_graph_merge = merge;
            std::vector<std::string> expected{
                "ClearX | SampleX: NonLocalRead(X)",
                "SampleX | AttachX: AttachmentReadInPass(X)",
            };
            if (merge)
            {
                expected.insert(expected.end(), {"AttachX | ClearXAgain+DepthA: ClearInPass(X)",
                                                 "ClearXAgain+DepthA | DepthB: DifferentDepth(D2)"});
            }
            else
            {
                expected.insert(expected.end(),
                                {"AttachX | ClearXAgain: ClearInPass(X)", "ClearXAgain | DepthA: Disabled",
                                 "DepthA | DepthB: DifferentDepth(D2)"});
            }
            expected.insert(expected.end(), {
                                                "DepthB | SlotY: SlotConflict(Y)",
                                                "SlotY | External: ExternalPass",
                                                "External | Readback: ExternalPass",
                                                "Readback | Readback: NonRasterPass",
                                            });

            const auto what = merge ? std::string("break reasons") : std::string("break reasons without merging");
            RGTexturePool pool(rhi);
            {
                RenderGraph graph(rhi, pool, merge_config);
                build(graph);
                graph.Compile();
                ExpectBreaks(graph, expected, what);
                Record(rhi, graph);
            }
            ExpectTexel(rhi, x_readback, {0, 255, 0, 255}, what + ": the last clear of X reaches its readback");
            ExpectTexel(rhi, y_readback, {0, 0, 255, 255}, what + ": the clear of Y reaches its readback");
        }

        // clears of two faces of a cube, then a texture sampled in two shader stages: a second barrier would share the
        // prologue batch of the first
        auto cube = CreateImage(rhi, "RenderGraphTestFaces", 8,
                                RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::Texture, 1,
                                RHIImage::ImageType::Image2DCube);
        RGTexturePool pool(rhi);
        {
            RenderGraph graph(rhi, pool, config);
            const auto faces = graph.Import("Cube", cube);
            const auto x = graph.CreateTexture("X", Rgba8Output);
            const auto y = graph.CreateTexture("Y", Rgba8Output);
            AddClear(graph, "ClearFace0", faces.Subresource(0, 0), 0, Vector4(1.f, 0.f, 0.f, 1.f));
            AddClear(graph, "ClearFace1", faces.Subresource(0, 1), 0, Vector4(0.f, 1.f, 0.f, 1.f));
            AddClear(graph, "ClearX", x, 0, Vector4(1.f, 0.f, 0.f, 1.f));
            graph.AddRasterPass("SamplePixel", [x, y](RGBuilder &builder) {
                builder.Sampled(x, RHIShaderStageMask::Pixel);
                builder.ColorWrite(y, 1, Vector4(0.f, 0.f, 1.f, 1.f));
                return [](RGRasterContext &) {};
            });
            graph.AddRasterPass("SampleVertex", [x, y](RGBuilder &builder) {
                builder.Sampled(x, RHIShaderStageMask::Vertex);
                builder.ColorWrite(y, 1);
                return [](RGRasterContext &) {};
            });
            AddReadback(graph, y, y_readback);
            graph.Compile();
            ExpectBreaks(graph,
                         {
                             "ClearFace0 | ClearFace1: SlotConflict(Cube)",
                             "ClearFace1 | ClearX: TargetSizeMismatch",
                             "ClearX | SamplePixel: NonLocalRead(X)",
                             "SamplePixel | SampleVertex: NonLocalRead(X)",
                             "SampleVertex | Readback: NonRasterPass",
                         },
                         "cube faces and shader stages");
            Record(rhi, graph);
        }
        ExpectTexel(rhi, y_readback, {0, 0, 255, 255}, "the texture sampled in two stages keeps its clear");
    }

    // a boundary that breaks several rules lists every one with the resources it names, and the opportunity report
    // weighs each physical pass by the bytes its attachment loads and stores move
    void OpportunityReport(RHIContext *rhi, const RenderConfig &config)
    {
        auto no_merge = config;
        no_merge.render_graph_merge = false;
        RGTexturePool pool(rhi);
        RenderGraph graph(rhi, pool, no_merge);
        const auto x = graph.CreateTexture("X", Rgba8Output);
        const auto y = graph.CreateTexture("Y", Rgba8Output);
        const auto d1 = graph.CreateTexture("D1", DepthOutput);
        const auto d2 = graph.CreateTexture("D2", DepthOutput);
        graph.AddRasterPass("WriteX", [x, d1](RGBuilder &builder) {
            builder.ColorWrite(x, 0, Vector4(1.f, 0.f, 0.f, 1.f));
            builder.DepthWrite(d1, 1.f);
            return [](RGRasterContext &) {};
        });
        graph.AddRasterPass("SampleX", [x, y, d2](RGBuilder &builder) {
            builder.Sampled(x);
            builder.ColorWrite(y, 0, Vector4(0.f, 0.f, 1.f, 1.f));
            builder.DepthWrite(d2, 1.f);
            builder.SideEffect();
            return [](RGRasterContext &) {};
        });
        graph.Compile();

        const auto dump = graph.Dump();
        const auto x_bytes = uint64_t{config.GetResolution().output.x()} * config.GetResolution().output.y() * 4;
        Expect(dump.at("physical_passes").at(0).at("breaks") ==
                   nlohmann::json::array({"DifferentDepth(D2)", "SlotConflict(Y)", "NonLocalRead(X)", "Disabled"}),
               "a boundary lists every rule it breaks");
        Expect(dump.at("physical_passes").at(0).at("break_reason") == "DifferentDepth",
               "the first rule broken is the break reason");
        Expect(dump.at("opportunities") ==
                   nlohmann::json::array(
                       {std::format("WriteX|SampleX: DifferentDepth(D2), SlotConflict(Y), NonLocalRead(X), Disabled; "
                                    "Store X {:.2f} MB",
                                    static_cast<double>(x_bytes) / 1e6),
                        "SampleX"}),
               "the opportunity report weighs the store of X");
        const auto &totals = dump.at("totals");
        Expect(totals.at("render_passes") == 2 && totals.at("load_bytes") == 0 && totals.at("store_bytes") == x_bytes,
               "the totals count the render passes and the bytes stored");
        if (HasFailed())
        {
            Log(Error, "dump: {}", dump.dump());
        }
        Record(rhi, graph);
    }

    // attachments over the tile budget warn in one physical pass, or split it with render_graph_tile_budget_split
    void TileBudget(RHIContext *rhi, const RenderConfig &config)
    {
        for (const bool split : {false, true})
        {
            auto budget_config = config;
            budget_config.render_graph_tile_budget = 4;
            budget_config.render_graph_tile_budget_split = split;
            RGTexturePool pool(rhi);
            RenderGraph graph(rhi, pool, budget_config);
            const auto x = graph.CreateTexture("X", Rgba8Output);
            const auto y = graph.CreateTexture("Y", Rgba8Output);
            for (const auto &[name, texture, slot] : {std::tuple{"WriteX", x, 0}, std::tuple{"WriteY", y, 1}})
            {
                graph.AddRasterPass(name, [texture, slot](RGBuilder &builder) {
                    builder.ColorWrite(texture, static_cast<uint8_t>(slot), Vector4(0.f, 0.f, 0.f, 1.f));
                    builder.SideEffect();
                    return [](RGRasterContext &) {};
                });
            }
            graph.Compile();

            const auto what = split ? std::string("tile budget split") : std::string("tile budget warning");
            const auto dump = graph.Dump();
            ExpectLines(Breaks(dump),
                        split ? std::vector<std::string>{"WriteX | WriteY: TileBudget"} : std::vector<std::string>{},
                        what, dump);
            const auto &physical = dump.at("physical_passes").at(0);
            Expect(dump.at("tile_budget") == 4 && physical.at("color_bytes_per_pixel") == (split ? 4 : 8) &&
                       physical.value("over_tile_budget", false) == !split,
                   what + ": the physical pass reports its color bytes per pixel against the budget");
            if (!split)
            {
                Expect(dump.at("opportunities") ==
                           nlohmann::json::array({"WriteY: 8 B/pixel over the 4 B tile budget"}),
                       what + ": the opportunity report warns");
            }
            Record(rhi, graph);
        }

        RGTexturePool pool(rhi);
        RenderGraph graph(rhi, pool, config);
        graph.Compile();
        const auto budget = rhi->GetTileBudget();
        Expect(budget ? graph.Dump().at("tile_budget") == *budget : !graph.Dump().contains("tile_budget"),
               "without render_graph_tile_budget the device's budget applies");
    }

    // a quad sampling a texture written before its physical pass, whose pipeline tests depth, joins a pass clearing
    // slot 1 and the depth to 0, where every depth test fails: the sampled texture's barrier is recorded before the
    // rendering, the quad's write mask leaves slot 1 untouched, and it neither tests nor writes the depth it does not
    // attach
    void MergedDraws(RHIContext *rhi, const RenderConfig &config)
    {
        const auto quad = PipelinePass::Create<DepthTestingQuadPass>(config, rhi, "Quad", Rgba8Output.format,
                                                                     ScreenQuadPass::InputFilter::Nearest);
        auto first_readback = CreateReadbackBuffer(rhi, config);
        auto second_readback = CreateReadbackBuffer(rhi, config);

        RGTexturePool pool(rhi);
        {
            RenderGraph graph(rhi, pool, config);
            const auto input = graph.CreateTexture("Input", Rgba8Scene);
            const auto first = graph.CreateTexture("First", Rgba8Output);
            const auto second = graph.CreateTexture("Second", Rgba8Output);
            const auto depth = graph.CreateTexture("Depth", DepthOutput);
            AddClear(graph, "ClearInput", input, 0, Vector4(0.f, 0.f, 1.f, 1.f));
            graph.AddRasterPass("ClearTargets", [second, depth](RGBuilder &builder) {
                builder.ColorWrite(second, 1, Vector4(0.f, 1.f, 0.f, 1.f));
                builder.DepthWrite(depth, 0.f);
                return [](RGRasterContext &) {};
            });
            quad->AddTo(graph, input, first);
            AddReadback(graph, first, first_readback);
            AddReadback(graph, second, second_readback);

            Run(rhi, graph,
                {
                    "ClearInput: Raster",
                    "  barrier Input Undefined->ColorOutput [None -> ColorWrite]",
                    "  attachment Input slot 0: Clear (clear) / Store (read by Quad)",
                    "ClearTargets: Raster",
                    "  barrier Second Undefined->ColorOutput [None -> ColorWrite]",
                    "  barrier Depth Undefined->DepthStencilOutput [None -> DepthWrite]",
                    "Quad: Raster, step 1",
                    "  barrier Input ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                    "  barrier First Undefined->ColorOutput [None -> ColorWrite]",
                    "  physical ClearTargets+Quad",
                    "  attachment Second slot 1: Clear (clear) / Store (read by Readback)",
                    "  attachment Depth slot depth: Clear (clear) / DontCare (no later reader)",
                    "  attachment First slot 0: DontCare (fully overwritten) / Store (read by Readback)",
                    "Readback: Copy",
                    "  barrier First ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                    "  barrier after Readback [CopyDst -> HostRead]",
                    "Readback: Copy",
                    "  barrier Second ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                    "  barrier after Readback [CopyDst -> HostRead]",
                    "Input: physical 0",
                    "First: physical 3",
                    "Second: physical 1",
                    "Depth: physical 2, memoryless",
                },
                "merged draws");
            ExpectBreaks(graph,
                         {
                             "ClearInput | ClearTargets+Quad: TargetSizeMismatch",
                             "ClearTargets+Quad | Readback: NonRasterPass",
                             "Readback | Readback: NonRasterPass",
                         },
                         "merged draws");
        }

        ExpectTexel(rhi, first_readback, {0, 0, 255, 255},
                    "the quad draws the texture sampled after its barrier, ignoring the depth");
        ExpectTexel(rhi, second_readback, {0, 255, 0, 255}, "the quad leaves the slot it does not write untouched");
    }

    // a transient last used by one member and one first used by a later member are attached together, so they get
    // pooled images of their own; unmerged, they share one
    void AliasingInPhysicalPass(RHIContext *rhi, const RenderConfig &config)
    {
        for (const bool merge : {true, false})
        {
            auto merge_config = config;
            merge_config.render_graph_merge = merge;
            merge_config.render_graph_memoryless = false;
            auto readback = CreateReadbackBuffer(rhi, config);
            RGTexturePool pool(rhi);
            {
                RenderGraph graph(rhi, pool, merge_config);
                const auto t1 = graph.CreateTexture("T1", Rgba8Output);
                const auto t2 = graph.CreateTexture("T2", Rgba8Output);
                graph.AddRasterPass("WriteT1", [t1](RGBuilder &builder) {
                    builder.ColorWrite(t1, 0, Vector4(1.f, 0.f, 0.f, 1.f));
                    builder.SideEffect();
                    return [](RGRasterContext &) {};
                });
                AddClear(graph, "WriteT2", t2, 1, Vector4(0.f, 1.f, 0.f, 1.f));
                AddReadback(graph, t2, readback);

                const std::vector<std::string> readback_lines{
                    "Readback: Copy",
                    "  barrier T2 ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                    "  barrier after Readback [CopyDst -> HostRead]",
                };
                auto expected = merge ? std::vector<std::string>{
                                            "WriteT1: Raster",
                                            "  barrier T1 Undefined->ColorOutput [None -> ColorWrite]",
                                            "WriteT2: Raster, step 1",
                                            "  barrier T2 Undefined->ColorOutput [None -> ColorWrite]",
                                            "  physical WriteT1+WriteT2",
                                            "  attachment T1 slot 0: Clear (clear) / DontCare (no later reader)",
                                            "  attachment T2 slot 1: Clear (clear) / Store (read by Readback)",
                                        }
                                      : std::vector<std::string>{
                                            "WriteT1: Raster",
                                            "  barrier T1 Undefined->ColorOutput [None -> ColorWrite]",
                                            "  attachment T1 slot 0: Clear (clear) / DontCare (no later reader)",
                                            "WriteT2: Raster",
                                            "  barrier T2 Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                                            "  attachment T2 slot 1: Clear (clear) / Store (read by Readback)",
                                        };
                expected.insert(expected.end(), readback_lines.begin(), readback_lines.end());
                expected.insert(expected.end(), {"T1: physical 0", merge ? "T2: physical 1" : "T2: physical 0"});
                Run(rhi, graph, expected, merge ? "aliasing in a physical pass" : "aliasing without merging");
            }
            ExpectTexel(rhi, readback, {0, 255, 0, 255}, "the second transient keeps its clear");
        }
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
                "Shown: physical 2, memoryless",
            },
            "fully overwrites with a read-write access");
    }

    // a transient only the attachments of one physical pass use is memoryless: Scratch, and Depth once its clear and
    // test merge. Target, of Scratch's format, is stored and read back. unmerged, Depth is stored for the quad, whose
    // depth test fails against its clear, and the pool gives it no memoryless image of the merged frame. with the knob
    // off, or Scratch sampled after the physical pass, they are not memoryless.
    void Memoryless(RHIContext *rhi, const RenderConfig &config)
    {
        const auto quad = PipelinePass::Create<DepthTestingQuadPass>(config, rhi, "Quad", Rgba8Output.format,
                                                                     ScreenQuadPass::InputFilter::Nearest);
        auto readback = CreateReadbackBuffer(rhi, config);
        const auto build = [&quad, &readback](RenderGraph &graph, bool view) {
            const auto input = graph.CreateTexture("Input", Rgba8Output);
            const auto target = graph.CreateTexture("Target", Rgba8Output);
            const auto scratch = graph.CreateTexture("Scratch", Rgba8Output);
            const auto depth = graph.CreateTexture("Depth", DepthOutput);
            AddClear(graph, "ClearInput", input, 0, Vector4(0.f, 0.f, 1.f, 1.f));
            graph.AddRasterPass("ClearTargets", [target, scratch, depth](RGBuilder &builder) {
                builder.ColorWrite(target, 0, Vector4(0.f, 1.f, 0.f, 1.f));
                builder.ColorWrite(scratch, 1, Vector4(1.f, 0.f, 0.f, 1.f));
                builder.DepthWrite(depth, 0.f);
                return [](RGRasterContext &) {};
            });
            quad->AddTo(graph, input, target, depth);
            if (view)
            {
                const auto viewed = graph.CreateTexture("Viewed", Rgba8Output);
                graph.AddRasterPass("View", [scratch, viewed](RGBuilder &builder) {
                    builder.Sampled(scratch);
                    builder.ColorWrite(viewed, 0, Vector4(0.f, 0.f, 0.f, 1.f));
                    builder.SideEffect();
                    return [](RGRasterContext &) {};
                });
            }
            AddReadback(graph, target, readback);
        };
        const auto run = [this, rhi, &config, &build, &readback](RGTexturePool &pool, bool merge, bool memoryless,
                                                                 bool view, const std::string &what) {
            auto case_config = config;
            case_config.render_graph_merge = merge;
            case_config.render_graph_memoryless = memoryless;
            nlohmann::json dump;
            {
                RenderGraph graph(rhi, pool, case_config);
                build(graph, view);
                graph.Compile();
                dump = graph.Dump();
                ExpectBackings(rhi, dump, what);
                Record(rhi, graph);
            }
            ExpectTexel(rhi, readback, {0, 255, 0, 255}, what + ": the quad fails the depth test against the clear");
            return dump;
        };
        // the resource lines of a summary
        const auto images = [](const nlohmann::json &dump) {
            auto lines = Summarize(dump);
            std::erase_if(lines, [](const std::string &line) { return line.find(": physical ") == std::string::npos; });
            return lines;
        };

        RGTexturePool pool(rhi);
        const auto merged = run(pool, true, true, false, "memoryless");
        ExpectLines(Summarize(merged),
                    {
                        "ClearInput: Raster",
                        "  barrier Input Undefined->ColorOutput [None -> ColorWrite]",
                        "  attachment Input slot 0: Clear (clear) / Store (read by Quad)",
                        "ClearTargets: Raster",
                        "  barrier Target Undefined->ColorOutput [None -> ColorWrite]",
                        "  barrier Scratch Undefined->ColorOutput [None -> ColorWrite]",
                        "  barrier Depth Undefined->DepthStencilOutput [None -> DepthWrite]",
                        "Quad: Raster, step 1",
                        "  barrier Input ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                        "  physical ClearTargets+Quad",
                        "  attachment Target slot 0: Clear (clear) / Store (read by Readback)",
                        "  attachment Scratch slot 1: Clear (clear) / DontCare (no later reader)",
                        "  attachment Depth slot depth: Clear (clear) / DontCare (no later reader)",
                        "Readback: Copy",
                        "  barrier Target ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                        "  barrier after Readback [CopyDst -> HostRead]",
                        "Input: physical 0",
                        "Target: physical 1",
                        "Scratch: physical 2, memoryless",
                        "Depth: physical 3, memoryless",
                    },
                    "memoryless plan", merged);

        // only a memoryless image of the merged frame is new to the stored Depth
        const auto unmerged = run(pool, false, true, false, "memoryless without merging");
        ExpectLines(Summarize(unmerged),
                    {
                        "ClearInput: Raster",
                        "  barrier Input Undefined->ColorOutput [Sampled(Pixel) -> ColorWrite]",
                        "  attachment Input slot 0: Clear (clear) / Store (read by Quad)",
                        "ClearTargets: Raster",
                        "  barrier Target Undefined->ColorOutput [CopySrc -> ColorWrite]",
                        "  barrier Scratch Undefined->ColorOutput [ColorWrite -> ColorWrite]",
                        rhi->SupportsMemorylessImage(DepthOutput.format, RHIImage::ImageUsage::DepthStencilAttachment)
                            ? "  barrier Depth Undefined->DepthStencilOutput [None -> DepthWrite]"
                            : "  barrier Depth Undefined->DepthStencilOutput [DepthWrite -> DepthWrite]",
                        "  attachment Target slot 0: Clear (clear) / Store (read by Quad)",
                        "  attachment Scratch slot 1: Clear (clear) / DontCare (no later reader)",
                        "  attachment Depth slot depth: Clear (clear) / Store (read by Quad)",
                        "Quad: Raster",
                        "  barrier Input ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
                        "  barrier Target ColorOutput->ColorOutput [ColorWrite -> ColorWrite]",
                        "  barrier Depth DepthStencilOutput->DepthStencilOutput [DepthWrite -> DepthWrite]",
                        "  attachment Target slot 0: Load (written by ClearTargets) / Store (read by Readback)",
                        "  attachment Depth slot depth: Load (written by ClearTargets) / DontCare (no later reader)",
                        "Readback: Copy",
                        "  barrier Target ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
                        "  barrier Readback [HostRead -> CopyDst]",
                        "  barrier after Readback [CopyDst -> HostRead]",
                        "Input: physical 0",
                        "Target: physical 1",
                        "Scratch: physical 2, memoryless",
                        "Depth: physical 3",
                    },
                    "memoryless without merging plan", unmerged);

        RGTexturePool off_pool(rhi);
        const auto off = run(off_pool, true, false, false, "memoryless off");
        ExpectLines(images(off),
                    {"Input: physical 0", "Target: physical 1", "Scratch: physical 2", "Depth: physical 3"},
                    "memoryless off images", off);

        RGTexturePool view_pool(rhi);
        const auto viewed = run(view_pool, true, true, true, "memoryless of a sampled texture");
        ExpectLines(images(viewed),
                    {"Input: physical 0", "Target: physical 1", "Scratch: physical 2", "Depth: physical 3, memoryless",
                     "Viewed: physical 4, memoryless"},
                    "memoryless of a sampled texture images", viewed);
    }

    // tone mapping reads a clear of an earlier member of its physical pass pixel-locally, recording the barrier its
    // read waits on inside the rendering, and the cleared texture stays in tile memory. with pixel-local reads off, or
    // a device without them, it samples the clear after its physical pass instead, and draws the same texels.
    void PixelLocalReadback(RHIContext *rhi, const RenderConfig &config)
    {
        constexpr RGTextureDesc ColorDesc{.format = PixelFormat::RGBAFloat16, .size_class = RGSizeClass::Output};
        const auto tone_mapping = PipelinePass::Create<ExposedToneMappingPass>(config, rhi, Rgba8Output.format);
        tone_mapping->SetExposure(1.f);

        const std::vector<std::string> local_plan{
            "WriteColor: Raster",
            "  barrier Color Undefined->LocalRead [None -> ColorWrite]",
            "ToneMapping: Raster, step 1",
            "  barrier in rendering Color LocalRead->LocalRead [ColorWrite -> PixelLocalRead(Pixel)]",
            "  barrier Out Undefined->ColorOutput [None -> ColorWrite]",
            "  physical WriteColor+ToneMapping",
            "  attachment Color slot 1: Clear (clear) / DontCare (no later reader)",
            "  attachment Out slot 0: DontCare (fully overwritten) / Store (read by Readback)",
        };
        const std::vector<std::string> sampled_plan{
            "WriteColor: Raster",
            "  barrier Color Undefined->ColorOutput [None -> ColorWrite]",
            "  attachment Color slot 1: Clear (clear) / Store (read by ToneMapping)",
            "ToneMapping: Raster",
            "  barrier Color ColorOutput->Read [ColorWrite -> Sampled(Pixel)]",
            "  barrier Out Undefined->ColorOutput [None -> ColorWrite]",
            "  attachment Out slot 0: DontCare (fully overwritten) / Store (read by Readback)",
        };
        const std::vector<std::string> readback_plan{
            "Readback: Copy",
            "  barrier Out ColorOutput->TransferSrc [ColorWrite -> CopySrc]",
            "  barrier after Readback [CopyDst -> HostRead]",
        };

        std::array<std::array<uint8_t, 4>, 2> texels{};
        for (const bool pixel_local : {true, false})
        {
            auto case_config = config;
            case_config.render_graph_pixel_local = pixel_local;
            const bool kept = pixel_local && rhi->SupportsPixelLocalRead();
            const auto what = std::format("pixel-local read {}", pixel_local ? "on" : "off");
            auto readback = CreateReadbackBuffer(rhi, config);
            RGTexturePool pool(rhi);
            {
                RenderGraph graph(rhi, pool, case_config);
                const auto color = graph.CreateTexture("Color", ColorDesc);
                const auto out = graph.CreateTexture("Out", Rgba8Output);
                AddClear(graph, "WriteColor", color, 1, Vector4(0.25f, 0.5f, 1.f, 1.f));
                tone_mapping->AddTo(graph, color, out);
                AddReadback(graph, out, readback);

                auto expected = kept ? local_plan : sampled_plan;
                expected.insert(expected.end(), readback_plan.begin(), readback_plan.end());
                expected.insert(expected.end(),
                                {kept ? "Color: physical 0, memoryless" : "Color: physical 0", "Out: physical 1"});
                Run(rhi, graph, expected, what);

                const auto dump = graph.Dump();
                ExpectLines(PixelLocalReads(dump),
                            {kept ? "ToneMapping Color: PixelLocalRead(Pixel)"
                                  : std::string("ToneMapping Color: lowered NoPixelLocalSupport(Color)")},
                            what + " request", dump);
            }

            rhi->WaitForDeviceIdle();
            const auto *pixel = static_cast<const uint8_t *>(readback->Lock());
            std::copy_n(pixel, 4, texels[pixel_local ? 0 : 1].begin());
            readback->UnLock();
        }

        Expect(texels[0] == texels[1], "the pixel-local and the sampled read tone map the same texel");
        Expect(texels[0][2] > texels[0][0] && texels[0][0] > 0, "tone mapping draws the clear it reads");
    }

    // each reason a pixel-local read request is lowered to Sampled for, among requests by passes that draw nothing: the
    // reason the physical pass of the texture's last writer ended, or no writer. requests whose writer attaches the
    // texture at the slot they read, in their physical pass, stay pixel-local.
    void PixelLocalLowering(RHIContext *rhi, const RenderConfig &config)
    {
        const auto compute_pass = rhi->CreateComputePass("RenderGraphTestCompute", false);
        auto imported = CreateImage(rhi, "RenderGraphTestPixelLocal", 8,
                                    RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::Texture);
        const auto build = [&compute_pass, &imported](RenderGraph &graph) {
            const auto read = [&graph](const std::string &name, RGTexture input, uint8_t slot, RGTexture output) {
                graph.AddRasterPass(name, [input, slot, output](RGBuilder &builder) {
                    builder.PixelLocalRead(input, slot);
                    builder.ColorWrite(output, 0);
                    builder.SideEffect();
                    return [](RGRasterContext &) {};
                });
            };
            const auto out = graph.CreateTexture("Out", Rgba8Output);
            const auto x = graph.CreateTexture("X", Rgba8Output);
            AddClear(graph, "WriteX", x, 1, Vector4(1.f, 0.f, 0.f, 1.f));
            read("ReadX", x, 1, out);

            const auto scene = graph.CreateTexture("Scene", Rgba8Scene);
            AddClear(graph, "WriteScene", scene, 1, Vector4(1.f, 0.f, 0.f, 1.f));
            read("ReadScene", scene, 1, out);

            const auto slot2 = graph.CreateTexture("Slot2", Rgba8Output);
            AddClear(graph, "WriteSlot2", slot2, 2, Vector4(1.f, 0.f, 0.f, 1.f));
            read("ReadSlot2", slot2, 1, out);

            const auto stored = graph.CreateTexture("Stored", Rgba8Output);
            graph.AddComputePass("StoreStored", compute_pass, [stored](RGBuilder &builder) {
                builder.StorageWrite(stored);
                return [](RGComputeContext &) {};
            });
            read("ReadStored", stored, 1, out);

            read("ReadImported", graph.Import("Imported", imported), 1, graph.CreateTexture("Small", Rgba8Scene));
        };

        for (const auto &[merge, pixel_local] : {std::pair{true, true}, std::pair{true, false}, std::pair{false, true}})
        {
            auto case_config = config;
            case_config.render_graph_merge = merge;
            case_config.render_graph_pixel_local = pixel_local;
            const bool supported = pixel_local && rhi->SupportsPixelLocalRead();
            const auto what = std::format("pixel-local lowering, merge {} pixel-local {}", merge, pixel_local);
            const auto same_physical = [supported, merge](const std::string &resource) {
                if (!supported)
                {
                    return std::format("lowered NoPixelLocalSupport({})", resource);
                }
                return merge ? std::string("PixelLocalRead(Pixel)") : std::string("lowered Disabled");
            };

            RGTexturePool pool(rhi);
            RenderGraph graph(rhi, pool, case_config);
            build(graph);
            graph.Compile();
            const auto dump = graph.Dump();
            ExpectLines(PixelLocalReads(dump),
                        {
                            "ReadX X: " + same_physical("X"),
                            "ReadScene Scene: lowered TargetSizeMismatch",
                            "ReadSlot2 Slot2: lowered SlotConflict(Slot2)",
                            "ReadStored Stored: lowered NonRasterPass",
                            "ReadImported Imported: lowered NoWriter",
                        },
                        what, dump);
            Record(rhi, graph);
        }
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
                "Shown: physical 1, memoryless",
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
    static constexpr std::array<Step, 27> Steps{
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
        &RenderGraphCompileTest::BreakReasons,
        &RenderGraphCompileTest::OpportunityReport,
        &RenderGraphCompileTest::TileBudget,
        &RenderGraphCompileTest::MergedDraws,
        &RenderGraphCompileTest::AliasingInPhysicalPass,
        &RenderGraphCompileTest::FullyOverwritesReadWrite,
        &RenderGraphCompileTest::Memoryless,
        &RenderGraphCompileTest::PixelLocalReadback,
        &RenderGraphCompileTest::PixelLocalLowering,
        &RenderGraphCompileTest::NextFrameReuse,
        &RenderGraphCompileTest::NextFrameReuse,
        &RenderGraphCompileTest::ReleaseUnused,
        &RenderGraphCompileTest::ScreenInputFilter,
    };

    size_t step_ = 0;
    std::atomic<bool> task_pending_{false};

    // only accessed from the render thread
    Resources resources_;
};

static TestCaseRegistrar<RenderGraphCompileTest> render_graph_compile_test_registrar("render_graph_compile");
} // namespace sparkle
