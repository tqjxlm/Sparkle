#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "renderer/graph/RGError.h"
#include "renderer/graph/RGTexturePool.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "rhi/RHI.h"

#include <atomic>
#include <format>
#include <functional>
#include <string>
#include <tuple>

namespace sparkle
{
namespace
{
// a resource table no pipeline has: its members only name bindings
class ErrorTable : public RHIShaderResourceTable
{
    USE_SHADER_RESOURCE(texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(storage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
};

// a screen quad whose pass declares its input, also declares a binding its pipeline has no table for, or leaves the
// input its pipeline still holds undeclared
class ErrorQuadPass : public ScreenQuadPass
{
public:
    enum class Input : uint8_t
    {
        Declared,
        ExtraBinding,
        Undeclared,
    };

    using ScreenQuadPass::ScreenQuadPass;

    void AddWith(RenderGraph &graph, RGTexture input, RGTexture output, Input declared, RGTexture extra = {})
    {
        input_ = declared;
        extra_ = extra;
        AddTo(graph, input, output);
    }

protected:
    void SampleInput(RGBuilder &builder, RGTexture input) const override
    {
        if (input_ == Input::Undeclared)
        {
            return;
        }
        ScreenQuadPass::SampleInput(builder, input);
        if (input_ == Input::ExtraBinding)
        {
            builder.Sampled(extra_, &ErrorTable::texture);
        }
    }

private:
    Input input_ = Input::Declared;
    RGTexture extra_;
};
} // namespace

// makes one mistake per render graph error with errors thrown instead of aborting, and expects each error's message
class RenderGraphErrorsTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (!started_)
        {
            started_ = true;
            auto config = app.GetRenderConfig();
            config.image_width = 64;
            config.image_height = 32;
            config.render_scale = 0.5f;
            config.render_graph_cull = true;
            config.render_graph_full_barriers = false;
            TaskManager::RunInRenderThread([this, rhi = app.GetRHI(), config] {
                const RGErrorsThrow errors_throw;
                MakeMistakes(rhi, config);
                done_.store(true, std::memory_order_release);
            });
            return Result::Pending;
        }

        if (!done_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }
        return failed_ ? Result::Fail : Result::Pass;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 1000;
    }

private:
    static constexpr PixelFormat Format = PixelFormat::R8G8B8A8Unorm;
    static constexpr RGTextureDesc OutputDesc{.format = Format, .size_class = RGSizeClass::Output};
    static constexpr RGTextureDesc SceneDesc{.format = Format, .size_class = RGSizeClass::Scene};

    void Expect(bool condition, const std::string &what)
    {
        if (condition)
        {
            Log(Info, "{}: OK - {}", GetName(), what);
            return;
        }

        Log(Error, "{}: FAILED - {}", GetName(), what);
        failed_ = true;
    }

    // runs `mistake`, which must throw the render graph error `expected`
    void ExpectError(const std::string &expected, const std::function<void()> &mistake)
    {
        try
        {
            mistake();
        }
        catch (const RGError &error)
        {
            Expect(error.what() == expected, std::format(R"("{}" (got "{}"))", expected, error.what()));
            return;
        }
        Expect(false, std::format(R"("{}" (no error))", expected));
    }

    // compiles the graph and records it into its own command buffer, submitted and finished even when recording throws.
    // a pass that throws leaves its bindings set, which must not outlive the graph.
    static void Run(RHIContext *rhi, RenderGraph &graph)
    {
        graph.Compile();
        rhi->BeginCommandBuffer();
        try
        {
            graph.Execute(*rhi->GetCommandContext());
        }
        catch (const RGError &)
        {
            rhi->GetCommandContext()->SetBindings({});
            rhi->SubmitCommandBuffer();
            rhi->WaitForDeviceIdle();
            throw;
        }
        rhi->SubmitCommandBuffer();
        rhi->WaitForDeviceIdle();
    }

    static RHIResourceRef<RHIImage> CreateImage(RHIContext *rhi, const std::string &name, RHIImage::ImageUsage usages,
                                                uint8_t mips = 1)
    {
        return rhi->CreateImage(
            {.format = Format, .sampler = {}, .width = 16, .height = 16, .usages = usages, .mip_levels = mips}, name);
    }

    static RHIResourceRef<RHIBuffer> CreateBuffer(RHIContext *rhi, const std::string &name,
                                                  RHIBuffer::BufferUsage usages)
    {
        return rhi->CreateBuffer({.size = 16u * 16u * 4u,
                                  .usages = usages,
                                  .mem_properties = RHIMemoryProperty::DeviceLocal,
                                  .is_dynamic = false},
                                 name);
    }

    void MakeMistakes(RHIContext *rhi, const RenderConfig &config)
    {
        using Usage = RHIImage::ImageUsage;
        const auto target = CreateImage(rhi, "Target", Usage::ColorAttachment | Usage::Texture | Usage::TransferDst);
        const auto mips = CreateImage(rhi, "Mips", Usage::ColorAttachment | Usage::Texture | Usage::UAV, 2);
        const auto second_target = CreateImage(rhi, "SecondTarget", Usage::ColorAttachment | Usage::Texture);
        const auto transitioned = CreateImage(rhi, "Transitioned", Usage::TransferDst);
        const auto sampled_only = CreateImage(rhi, "SampledOnly", Usage::Texture);
        const auto staging =
            CreateBuffer(rhi, "Staging", RHIBuffer::BufferUsage::TransferSrc | RHIBuffer::BufferUsage::TransferDst);
        const auto upload_only = CreateBuffer(rhi, "UploadOnly", RHIBuffer::BufferUsage::TransferDst);
        const auto quad = PipelinePass::Create<ErrorQuadPass>(config, rhi, "Quad", Format);

        RGTexturePool pool(rhi);

        // creating and importing
        ExpectError("Empty has no format or size", [&] {
            RenderGraph graph(rhi, pool, config);
            std::ignore = graph.CreateTexture("Empty", {});
        });
        ExpectError("import NoImage is not a single-sampled image", [&] {
            RenderGraph graph(rhi, pool, config);
            std::ignore = graph.Import("NoImage", RHIResourceRef<RHIImage>());
        });
        ExpectError("import NoBuffer is not a buffer", [&] {
            RenderGraph graph(rhi, pool, config);
            std::ignore = graph.Import("NoBuffer", RHIResourceRef<RHIBuffer>());
        });
        ExpectError("import NoTLAS is not an acceleration structure", [&] {
            RenderGraph graph(rhi, pool, config);
            std::ignore = graph.Import("NoTLAS", RHIResourceRef<RHITLAS>());
        });

        // declaring
        ExpectError("pass Invalid declares an invalid texture", [&] {
            RenderGraph graph(rhi, pool, config);
            graph.AddRasterPass("Invalid", [](RGBuilder &builder) {
                builder.ColorWrite(RGTexture{}, 0);
                return [](RGRasterContext &) {};
            });
        });
        ExpectError("pass Invalid declares an invalid buffer", [&] {
            RenderGraph graph(rhi, pool, config);
            graph.AddCopyPass("Invalid", [](RGBuilder &builder) {
                builder.CopySrc(RGBuffer{});
                return [](RGCopyContext &) {};
            });
        });
        ExpectError("pass Sample declares subresources A does not have", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto a = graph.CreateTexture("A", OutputDesc);
            graph.AddRasterPass("Sample", [a](RGBuilder &builder) {
                builder.Sampled(a.Mip(1));
                return [](RGRasterContext &) {};
            });
        });
        ExpectError("pass Attach attaches more than one subresource of Mips", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto texture = graph.Import("Mips", mips);
            graph.AddRasterPass("Attach", [texture](RGBuilder &builder) {
                builder.ColorWrite(texture, 0);
                return [](RGRasterContext &) {};
            });
        });
        ExpectError("a Compute pass cannot declare this access to A", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto a = graph.CreateTexture("A", OutputDesc);
            graph.AddComputePass("Compute", nullptr, [a](RGBuilder &builder) {
                builder.ColorWrite(a, 0);
                return [](RGComputeContext &) {};
            });
        });
        ExpectError("pass Twice declares Target twice", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto texture = graph.Import("Target", target);
            graph.AddRasterPass("Twice", [texture](RGBuilder &builder) {
                builder.Sampled(texture);
                builder.Sampled(texture);
                return [](RGRasterContext &) {};
            });
        });
        ExpectError("pass Slots binds two attachments to slot 0", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto a = graph.CreateTexture("A", OutputDesc);
            const auto b = graph.CreateTexture("B", OutputDesc);
            graph.AddRasterPass("Slots", [a, b](RGBuilder &builder) {
                builder.ColorWrite(a, 0);
                builder.ColorWrite(b, 0);
                return [](RGRasterContext &) {};
            });
        });
        ExpectError(std::format("color slot {} out of range", MaxNumColorAttachments), [&] {
            RenderGraph graph(rhi, pool, config);
            const auto a = graph.CreateTexture("A", OutputDesc);
            graph.AddRasterPass("Slot", [a](RGBuilder &builder) {
                builder.ColorWrite(a, MaxNumColorAttachments);
                return [](RGRasterContext &) {};
            });
        });
        ExpectError("Copy pass Native cannot declare native access", [&] {
            RenderGraph graph(rhi, pool, config);
            graph.AddCopyPass("Native", [](RGBuilder &builder) {
                builder.NativeAccess();
                return [](RGCopyContext &) {};
            });
        });

        // compiling
        ExpectError("the graph compiles once", [&] {
            RenderGraph graph(rhi, pool, config);
            graph.Compile();
            graph.Compile();
        });
        ExpectError("a texture pool serves one graph at a time", [&] {
            RenderGraph first(rhi, pool, config);
            RenderGraph second(rhi, pool, config);
            first.Compile();
            second.Compile();
        });
        ExpectError("raster pass Raster has no attachment", [&] {
            RenderGraph graph(rhi, pool, config);
            graph.AddRasterPass("Raster", [](RGBuilder &builder) {
                builder.SideEffect();
                return [](RGRasterContext &) {};
            });
            graph.Compile();
        });
        ExpectError("compute pass Compute has no compute pass", [&] {
            RenderGraph graph(rhi, pool, config);
            graph.AddComputePass("Compute", nullptr, [](RGBuilder &builder) {
                builder.SideEffect();
                return [](RGComputeContext &) {};
            });
            graph.Compile();
        });
        ExpectError("pass Read reads A before any pass writes it", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto a = graph.CreateTexture("A", OutputDesc);
            const auto output = graph.Import("Target", target);
            graph.AddRasterPass("Read", [a, output](RGBuilder &builder) {
                builder.Sampled(a);
                builder.ColorWrite(output, 0);
                return [](RGRasterContext &) {};
            });
            graph.Compile();
        });
        ExpectError("import SampledOnly lacks the usages its accesses need", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto texture = graph.Import("SampledOnly", sampled_only);
            graph.AddRasterPass("Write", [texture](RGBuilder &builder) {
                builder.ColorWrite(texture, 0);
                return [](RGRasterContext &) {};
            });
            graph.Compile();
        });
        ExpectError("import UploadOnly lacks the usages its accesses need", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto buffer = graph.Import("UploadOnly", upload_only);
            const auto texture = graph.Import("Target", target);
            graph.AddCopyPass("Upload", [buffer, texture](RGBuilder &builder) {
                builder.CopySrc(buffer);
                builder.CopyDst(texture);
                return [](RGCopyContext &) {};
            });
            graph.Compile();
        });
        ExpectError("attachments of pass Sizes differ in size", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto output = graph.CreateTexture("Output", OutputDesc);
            const auto scene = graph.CreateTexture("Scene", SceneDesc);
            graph.AddRasterPass("Sizes", [output, scene](RGBuilder &builder) {
                builder.ColorWrite(output, 0);
                builder.ColorWrite(scene, 1);
                builder.SideEffect();
                return [](RGRasterContext &) {};
            });
            graph.Compile();
        });
        ExpectError("a sampled binding of Mips views every subresource", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto texture = graph.Import("Mips", mips);
            const auto output = graph.Import("Target", target);
            graph.AddRasterPass("Sample", [texture, output](RGBuilder &builder) {
                builder.Sampled(texture.Mip(0), &ErrorTable::texture);
                builder.ColorWrite(output, 0);
                return [](RGRasterContext &) {};
            });
            graph.Compile();
        });
        ExpectError("a storage binding of Mips views one mip", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto texture = graph.Import("Mips", mips);
            graph.AddExternalPass("Store", [texture](RGBuilder &builder) {
                builder.StorageWrite(texture, &ErrorTable::storage);
                return [](RGExternalContext &) {};
            });
            graph.Compile();
        });

        // executing
        ExpectError("the graph executes once, after compiling", [&] {
            RenderGraph graph(rhi, pool, config);
            graph.Execute(*rhi->GetCommandContext());
        });
        ExpectError("pass Download uses a texture it did not declare", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto undeclared = graph.Import("Target", target);
            const auto buffer = graph.Import("Staging", staging);
            graph.AddCopyPass("Download", [undeclared, buffer](RGBuilder &builder) {
                builder.CopyDst(buffer);
                return [undeclared, buffer](RGCopyContext &context) { context.CopyToBuffer(undeclared, buffer); };
            });
            Run(rhi, graph);
        });
        ExpectError("pass Upload uses a buffer it did not declare", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto texture = graph.Import("Target", target);
            const auto undeclared = graph.Import("Staging", staging);
            graph.AddCopyPass("Upload", [texture, undeclared](RGBuilder &builder) {
                builder.CopyDst(texture);
                return [texture, undeclared](RGCopyContext &context) { context.CopyFromBuffer(undeclared, texture); };
            });
            Run(rhi, graph);
        });
        ExpectError("pass Transition left Transitioned in layout Read with accesses beyond its declaration", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto texture = graph.Import("Transitioned", transitioned);
            graph.AddExternalPass("Transition", [texture](RGBuilder &builder) {
                builder.CopyDst(texture);
                return [texture](RGExternalContext &context) {
                    context.GetImage(texture)->SetState(
                        {.layout = RHIImageLayout::Read,
                         .access = {.access = RHIAccess::Sampled, .stages = RHIShaderStageMask::Pixel}},
                        0, 1, 0, 1);
                };
            });
            Run(rhi, graph);
        });
        ExpectError("pass Quad binds Extra to a resource table no pipeline it drew or dispatched has", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto input = graph.CreateTexture("Input", OutputDesc);
            const auto extra = graph.Import("Extra", sampled_only);
            const auto output = graph.Import("Target", target);
            AddClear(graph, input);
            quad->AddWith(graph, input, output, ErrorQuadPass::Input::ExtraBinding, extra);
            Run(rhi, graph);
        });
        ExpectError("pass Quad binds Input to screenTexture without declaring the access that binding makes", [&] {
            RenderGraph graph(rhi, pool, config);
            const auto input = graph.CreateTexture("Input", OutputDesc);
            const auto output = graph.Import("Target", target);
            const auto second_output = graph.Import("SecondTarget", second_target);
            AddClear(graph, input);
            quad->AddWith(graph, input, output, ErrorQuadPass::Input::Declared);
            quad->AddWith(graph, input, second_output, ErrorQuadPass::Input::Undeclared);
            Run(rhi, graph);
        });
    }

    static void AddClear(RenderGraph &graph, RGTexture texture)
    {
        graph.AddRasterPass("Clear", [texture](RGBuilder &builder) {
            builder.ColorWrite(texture, 0, Vector4(1.f, 0.f, 0.f, 1.f));
            return [](RGRasterContext &) {};
        });
    }

    bool started_ = false;
    std::atomic<bool> done_{false};
    // only accessed from the render thread until done_
    bool failed_ = false;
};

static TestCaseRegistrar<RenderGraphErrorsTest> render_graph_errors_test_registrar("render_graph_errors");
} // namespace sparkle
