#include "renderer/pass/PostChain.h"

#include "core/Path.h"
#include "core/ThreadManager.h"
#include "io/Image.h"
#include "renderer/RenderConfig.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/pass/ToneMappingPass.h"
#include "renderer/pass/UiPass.h"
#include "rhi/RHI.h"

#include <filesystem>
#include <utility>

namespace sparkle
{
namespace
{
// a float Texture2D samples the default view of an image of `format`: integer formats need an integer texture, and a
// depth-stencil view cannot be sampled
bool SamplesAsFloat(PixelFormat format)
{
    return format != PixelFormat::R32UInt && format != PixelFormat::RGBAUInt32 && format != PixelFormat::D24S8;
}

// a staging buffer for an image of `format` and `size`, saved to `file_path` once the frame completes, which then calls
// `on_saved`. the caller records the copy.
RHIResourceRef<RHIBuffer> CreateScreenshotBuffer(RHIContext *rhi, PixelFormat format, Vector2UInt size,
                                                 std::string file_path, std::function<void()> on_saved)
{
    auto staging_buffer =
        rhi->CreateBuffer({.size = GetImageMipByteSize(format, size.x(), size.y()),
                           .usages = RHIBuffer::BufferUsage::TransferDst,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "ScreenshotReadbackStagingBuffer");

    const auto width = size.x();
    const auto height = size.y();

    rhi->EnqueueEndOfFrameTasks([rhi, staging_buffer, width, height, format, output_path = std::move(file_path),
                                 on_complete = std::move(on_saved)]() {
        rhi->WaitForDeviceIdle();

        const auto *raw_data = reinterpret_cast<const uint8_t *>(staging_buffer->Lock());
        auto pixels = Image2D::CreateFromRawPixels(raw_data, width, height, format);
        staging_buffer->UnLock();
        bool success = pixels.WriteToFile(Path::External(output_path));

        if (success)
        {
            Log(Info, "Screenshot saved to {}", output_path);
        }
        else
        {
            Log(Error, "Failed to save screenshot to {}", output_path);
        }

        if (on_complete)
        {
            on_complete();
        }
    });

    return staging_buffer;
}
} // namespace

PostChain::PostChain(const RenderConfig &config, RHIContext *rhi, PixelFormat screen_format, ScreenPass screen_pass)
    : config_(config), rhi_(rhi), screen_desc_{.format = screen_format, .size_class = RGSizeClass::Output}
{
    if (!rhi_->IsHeadless())
    {
        ui_pass_ = std::make_unique<UiPass>(rhi_, screen_format);
    }

    present_pass_ =
        PipelinePass::Create<ScreenQuadPass>(config_, rhi_, "Present", rhi_->GetBackBuffer()->GetAttributes().format,
                                             ScreenQuadPass::InputFilter::NearestAtIntegerScale, true);

    graph_view_pass_ = PipelinePass::Create<ScreenQuadPass>(config_, rhi_, GraphViewPassName, screen_format,
                                                            ScreenQuadPass::InputFilter::Bilinear);

    switch (screen_pass)
    {
    case ScreenPass::None:
        break;
    case ScreenPass::ToneMapping:
        screen_pass_ = PipelinePass::Create<ToneMappingPass>(config_, rhi_, screen_format);
        break;
    case ScreenPass::Upsample:
        screen_pass_ = PipelinePass::Create<ScreenQuadPass>(config_, rhi_, "Upsample", screen_format,
                                                            ScreenQuadPass::InputFilter::Bilinear);
        break;
    default:
        UnImplemented(screen_pass);
        break;
    }
}

PostChain::~PostChain() = default;

void PostChain::RequestScreenshot(const std::string &file_path, bool capture_ui, std::function<void()> on_complete)
{
    ASSERT(ThreadManager::IsInRenderThread());
    ASSERT(!file_path.empty());

    std::filesystem::path screenshot_path = std::filesystem::path("screenshots") / file_path;
    screenshot_path.replace_extension(".png");

    pending_screenshot_ = PendingScreenshot{
        .file_path = screenshot_path.string(), .capture_ui = capture_ui, .on_complete = std::move(on_complete)};
}

void PostChain::UpdateFrameData(SceneRenderProxy *scene)
{
    if (screen_pass_)
    {
        screen_pass_->UpdateFrameData(config_, scene);
    }

    present_pass_->UpdateFrameData(config_, scene);
}

void PostChain::AddReadback(RenderGraph &graph, RGTexture texture, bool capture_ui)
{
    ASSERT(ThreadManager::IsInRenderThread());

    if (!pending_screenshot_ || pending_screenshot_->capture_ui != capture_ui)
    {
        return;
    }

    auto request = std::move(*pending_screenshot_);
    pending_screenshot_.reset();

    const auto staging_buffer = graph.Import(
        "ScreenshotBuffer", CreateScreenshotBuffer(rhi_, graph.GetFormat(texture), graph.GetSize(texture),
                                                   std::move(request.file_path), std::move(request.on_complete)));
    graph.ReadOnHost(staging_buffer);
    graph.AddCopyPass("Readback", [texture, staging_buffer](RGBuilder &builder) {
        builder.CopySrc(texture);
        builder.CopyDst(staging_buffer);
        return [texture, staging_buffer](RGCopyContext &context) { context.CopyToBuffer(texture, staging_buffer); };
    });
}

RGTexture PostChain::FindGraphView(const RenderGraph &graph)
{
    const auto &name = config_.render_graph_view;
    if (name != graph_view_)
    {
        graph_view_ = name;
        graph_view_warned_ = false;
    }

    if (name.empty())
    {
        return {};
    }

    if (const auto texture = graph.FindTexture(name);
        texture.IsValid() && graph.CanSample2D(texture) && SamplesAsFloat(graph.GetFormat(texture)))
    {
        return texture;
    }

    if (!graph_view_warned_)
    {
        Log(Warn, "render_graph_view {} is not a 2D color texture this frame's graph can sample. showing the frame",
            name);
        graph_view_warned_ = true;
    }
    return {};
}

void PostChain::AddTo(RenderGraph &graph, RGTexture scene)
{
    const ScreenQuadPass *screen_pass = screen_pass_.get();
    if (const auto view = FindGraphView(graph); view.IsValid())
    {
        scene = view;
        screen_pass = graph_view_pass_.get();
    }

    auto screen = scene;
    if (screen_pass)
    {
        screen = graph.CreateTexture("Screen", screen_desc_);
        screen_pass->AddTo(graph, scene, screen);
    }

    AddReadback(graph, screen, false);

    if (config_.render_ui && ui_pass_)
    {
        ui_pass_->AddTo(graph, screen);
    }

    // a screenshot with ui is served without it when the ui does not draw
    AddReadback(graph, screen, true);

    const auto back_buffer = graph.Import("BackBuffer", rhi_->GetBackBuffer());
    present_pass_->AddTo(graph, screen, back_buffer);
}
} // namespace sparkle
