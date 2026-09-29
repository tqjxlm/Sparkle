#include "renderer/renderer/Renderer.h"

#include "core/FileManager.h"
#include "core/Path.h"
#include "core/ThreadManager.h"
#include "io/Image.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/pass/UiPass.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/renderer/CPURenderer.h"
#include "renderer/renderer/DeferredRenderer.h"
#include "renderer/renderer/ForwardRenderer.h"
#include "renderer/renderer/GPURenderer.h"
#include "renderer/resource/ImageBasedLighting.h"
#include "rhi/RHI.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <utility>

namespace sparkle
{
std::unique_ptr<Renderer> Renderer::CreateRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                                   SceneRenderProxy *scene_render_proxy,
                                                   RGTexturePool &graph_texture_pool)
{
    ASSERT(ThreadManager::IsInRenderThread());

    std::unique_ptr<Renderer> renderer;

    switch (render_config.pipeline)
    {
    case RenderConfig::Pipeline::Cpu:
        renderer = std::make_unique<CPURenderer>(render_config, rhi_context, scene_render_proxy, graph_texture_pool);
        break;
    case RenderConfig::Pipeline::Gpu:
        renderer = std::make_unique<GPURenderer>(render_config, rhi_context, scene_render_proxy, graph_texture_pool);
        break;
    case RenderConfig::Pipeline::Forward:
        renderer =
            std::make_unique<ForwardRenderer>(render_config, rhi_context, scene_render_proxy, graph_texture_pool);
        break;
    case RenderConfig::Pipeline::Deferred:
        renderer =
            std::make_unique<DeferredRenderer>(render_config, rhi_context, scene_render_proxy, graph_texture_pool);
        break;
    default:
        UnImplemented(render_config.pipeline);
        break;
    }

    rhi_context->BeginCommandBuffer();

    renderer->InitRenderResources();

    rhi_context->SubmitCommandBuffer();

    // the first initialization is very special. it's better to wait for completion.
    rhi_context->WaitForDeviceIdle();

    return renderer;
}

Renderer::Renderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy,
                   RGTexturePool &graph_texture_pool)
    : rhi_(rhi_context), scene_render_proxy_(scene_render_proxy), resolution_(render_config.GetResolution()),
      render_config_(render_config), graph_texture_pool_(graph_texture_pool), graph_pass_timers_(rhi_context)
{
    Log(Info, "View size [{}, {}]", resolution_.output.x(), resolution_.output.y());

    if (resolution_.NeedUpsample())
    {
        Log(Info, "Scene renders at [{}, {}] (render_scale {})", resolution_.scene.x(), resolution_.scene.y(),
            render_config.render_scale);
    }
}

Renderer::~Renderer() = default;

void Renderer::Tick()
{
    scene_render_proxy_->Update(rhi_, *scene_render_proxy_->GetCamera(), render_config_);

    Update();

    present_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    scene_render_proxy_->EndUpdate(rhi_);
}

void Renderer::OnFrameBufferResize(int width, int height)
{
    rhi_->RecreateFrameBuffer(width, height);
}

void Renderer::NotifySceneLoaded()
{
    scene_loaded_ = true;
    Log(Info, "Renderer notified: scene loaded");
}

bool Renderer::IsReadyForAutoScreenshot() const
{
    return scene_loaded_ && !HasPendingAsyncTasks();
}

void Renderer::RequestSaveScreenshot(const std::string &file_path, bool capture_ui,
                                     Renderer::ScreenshotCallback on_complete)
{
    ASSERT(ThreadManager::IsInRenderThread());
    ASSERT(!file_path.empty());

    std::filesystem::path screenshot_path = std::filesystem::path("screenshots") / file_path;
    screenshot_path.replace_extension(".png");

    pending_screenshot_ = PendingScreenshot{
        .file_path = screenshot_path.string(), .capture_ui = capture_ui, .on_complete = std::move(on_complete)};
}

void Renderer::RequestGraphDump(const std::string &name, std::function<void()> on_complete)
{
    ASSERT(ThreadManager::IsInRenderThread());
    ASSERT(!name.empty());

    graph_dump_path_ = (std::filesystem::path("screenshots") / (name + ".json")).string();
    graph_dump_completion_ = std::move(on_complete);
}

void Renderer::RequestGraphDump(std::function<void(const nlohmann::json &)> on_dump)
{
    ASSERT(ThreadManager::IsInRenderThread());

    graph_dump_consumer_ = std::move(on_dump);
}

std::optional<Renderer::PendingScreenshot> Renderer::TakeScreenshotRequest(bool capture_ui)
{
    ASSERT(ThreadManager::IsInRenderThread());

    if (!pending_screenshot_ || pending_screenshot_->capture_ui != capture_ui)
    {
        return std::nullopt;
    }

    return std::exchange(pending_screenshot_, std::nullopt);
}

RHIResourceRef<RHIBuffer> Renderer::CreateScreenshotBuffer(PixelFormat format, Vector2UInt size,
                                                           PendingScreenshot screenshot)
{
    auto staging_buffer =
        rhi_->CreateBuffer({.size = GetImageMipByteSize(format, size.x(), size.y()),
                            .usages = RHIBuffer::BufferUsage::TransferDst,
                            .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                            .is_dynamic = false},
                           "ScreenshotReadbackStagingBuffer");

    const auto width = size.x();
    const auto height = size.y();
    auto *rhi = rhi_;

    rhi_->EnqueueEndOfFrameTasks([rhi, staging_buffer, width, height, format,
                                  output_path = std::move(screenshot.file_path),
                                  on_complete = std::move(screenshot.on_complete)]() {
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

void Renderer::AddReadback(RenderGraph &graph, RGTexture texture, bool capture_ui)
{
    auto request = TakeScreenshotRequest(capture_ui);
    if (!request)
    {
        return;
    }

    const auto staging_buffer =
        graph.Import("ScreenshotBuffer",
                     CreateScreenshotBuffer(graph.GetFormat(texture), graph.GetSize(texture), std::move(*request)));
    graph.AddCopyPass("Readback", [texture, staging_buffer](RGBuilder &builder) {
        builder.CopySrc(texture);
        builder.CopyDst(staging_buffer);
        return [texture, staging_buffer](RGCopyContext &context) { context.CopyToBuffer(texture, staging_buffer); };
    });
}

Renderer::SkyBoxMap Renderer::GetSkyBoxMap(RenderConfig::OutputImage mode, const ImageBasedLighting *ibl,
                                           const RHIResourceRef<RHIImage> &sky_map)
{
    RHIResourceRef<RHIImage> ibl_map;
    if (ibl && mode == RenderConfig::OutputImage::IBLDiffuseMap)
    {
        ibl_map = ibl->GetDiffuseMap();
    }
    else if (ibl && mode == RenderConfig::OutputImage::IBLSpecularMap)
    {
        ibl_map = ibl->GetSpecularMap();
    }
    return ibl_map ? SkyBoxMap{.image = ibl_map, .sampler = ImageBasedLighting::MapSampler}
                   : SkyBoxMap{.image = sky_map, .sampler = SkyRenderProxy::SkyMapSampler};
}

void Renderer::InitPostChain(const RGTextureDesc &screen_desc)
{
    screen_desc_ = screen_desc;

    if (!rhi_->IsHeadless())
    {
        ui_pass_ = std::make_unique<UiPass>(rhi_, screen_desc.format);
    }

    present_pass_ = PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, "Present",
                                                         rhi_->GetBackBuffer()->GetAttributes().format,
                                                         ScreenQuadPass::InputFilter::Nearest, true);

    graph_view_pass_ = PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, GraphViewPassName, screen_desc.format,
                                                            ScreenQuadPass::InputFilter::Bilinear);
}

// a float Texture2D samples the default view of an image of `format`: integer formats need an integer texture, and a
// depth-stencil view cannot be sampled
static bool SamplesAsFloat(PixelFormat format)
{
    return format != PixelFormat::R32UInt && format != PixelFormat::RGBAUInt32 && format != PixelFormat::D24S8;
}

RGTexture Renderer::FindGraphView(const RenderGraph &graph)
{
    const auto &name = render_config_.render_graph_view;
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

void Renderer::AddPostChain(RenderGraph &graph, RGTexture scene, const ScreenQuadPass *screen_pass)
{
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

    if (render_config_.render_ui && ui_pass_)
    {
        ui_pass_->AddTo(graph, screen);
    }

    // a screenshot with ui is served without it when the ui does not draw
    AddReadback(graph, screen, true);

    const auto back_buffer = graph.Import("BackBuffer", rhi_->GetBackBuffer());
    present_pass_->AddTo(graph, screen, back_buffer);
}

void Renderer::ExecuteGraph(RenderGraph &graph)
{
    ASSERT(ThreadManager::IsInRenderThread());

    graph.Compile();
    graph.Execute(*rhi_->GetCommandContext(), &graph_pass_timers_);

    if (graph_dump_path_.empty() && !graph_dump_consumer_)
    {
        return;
    }

    const auto dump = graph.Dump();

    if (const auto on_dump = std::exchange(graph_dump_consumer_, nullptr))
    {
        on_dump(dump);
    }

    if (graph_dump_path_.empty())
    {
        return;
    }

    const auto text = dump.dump(4);
    const auto saved_path =
        FileManager::GetNativeFileManager()->Write(Path::External(graph_dump_path_), text.data(), text.size());
    if (saved_path.empty())
    {
        Log(Error, "Failed to save render graph dump to {}", graph_dump_path_);
    }
    else
    {
        Log(Info, "Render graph dump saved to {}", graph_dump_path_);
    }

    graph_dump_path_.clear();
    if (const auto on_complete = std::exchange(graph_dump_completion_, nullptr))
    {
        on_complete();
    }
}
} // namespace sparkle
