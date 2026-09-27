#include "renderer/renderer/Renderer.h"

#include "core/FileManager.h"
#include "core/Path.h"
#include "core/ThreadManager.h"
#include "io/Image.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/DepthPass.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/pass/SkyBoxPass.h"
#include "renderer/pass/UiPass.h"
#include "renderer/proxy/SceneRenderProxy.h"
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
                                                   SceneRenderProxy *scene_render_proxy)
{
    ASSERT(ThreadManager::IsInRenderThread());

    std::unique_ptr<Renderer> renderer;

    switch (render_config.pipeline)
    {
    case RenderConfig::Pipeline::Cpu:
        renderer = std::make_unique<CPURenderer>(render_config, rhi_context, scene_render_proxy);
        break;
    case RenderConfig::Pipeline::Gpu:
        renderer = std::make_unique<GPURenderer>(render_config, rhi_context, scene_render_proxy);
        break;
    case RenderConfig::Pipeline::Forward:
        renderer = std::make_unique<ForwardRenderer>(render_config, rhi_context, scene_render_proxy);
        break;
    case RenderConfig::Pipeline::Deferred:
        renderer = std::make_unique<DeferredRenderer>(render_config, rhi_context, scene_render_proxy);
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

Renderer::Renderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy)
    : rhi_(rhi_context), scene_render_proxy_(scene_render_proxy), resolution_(render_config.GetResolution()),
      render_config_(render_config), graph_texture_pool_(rhi_context)
{
    Log(Info, "View size [{}, {}]", resolution_.output.x(), resolution_.output.y());

    if (resolution_.NeedUpsample())
    {
        Log(Info, "Scene renders at [{}, {}] (render_scale {})", resolution_.scene.x(), resolution_.scene.y(),
            render_config.render_scale);
    }
}

void Renderer::Tick()
{
    scene_render_proxy_->Update(rhi_, *scene_render_proxy_->GetCamera(), render_config_);

    Update();

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

std::optional<Renderer::PendingScreenshot> Renderer::TakeScreenshotRequest(bool capture_ui)
{
    ASSERT(ThreadManager::IsInRenderThread());

    if (!pending_screenshot_ || pending_screenshot_->capture_ui != capture_ui)
    {
        return std::nullopt;
    }

    return std::exchange(pending_screenshot_, std::nullopt);
}

RHIResourceRef<RHIBuffer> Renderer::CreateScreenshotBuffer(const RHIImage &image, PendingScreenshot screenshot)
{
    auto staging_buffer =
        rhi_->CreateBuffer({.size = image.GetStorageSize(),
                            .usages = RHIBuffer::BufferUsage::TransferDst,
                            .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                            .is_dynamic = false},
                           "ScreenshotReadbackStagingBuffer");

    const auto width = image.GetWidth();
    const auto height = image.GetHeight();
    const auto format = image.GetAttributes().format;
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

    graph.AddCopyPass("Readback", [this, texture, screenshot = std::move(*request)](RGBuilder &builder) {
        builder.CopySrc(texture);
        builder.SideEffect();
        return [this, texture, screenshot](RGCopyContext &context) {
            auto staging_buffer = CreateScreenshotBuffer(*context.GetImage(texture), screenshot);
            context.CopyToBuffer(texture, staging_buffer.get());
        };
    });
}

RGTexture Renderer::AddDirectionalShadowPass(RenderGraph &graph, DepthPass &shadow_pass)
{
    const auto shadow_map = graph.Import("ShadowMap", shadow_pass.GetOutput()->GetDepthImage());
    graph.AddExternalPass("DirectionalShadow", [&shadow_pass, shadow_map](RGBuilder &builder) {
        builder.DepthWrite(shadow_map, 1.f);
        return [&shadow_pass](RGExternalContext &) { shadow_pass.Render(); };
    });
    return shadow_map;
}

void Renderer::ImportIblMaps(RenderGraph &graph, const ImageBasedLighting &ibl, std::vector<RGTexture> &textures)
{
    for (const auto &[name, map] :
         {std::pair{"IblBrdf", ibl.GetBRDFMap()}, std::pair{"IblDiffuse", ibl.GetDiffuseMap()},
          std::pair{"IblSpecular", ibl.GetSpecularMap()}})
    {
        if (map)
        {
            textures.push_back(graph.Import(name, map));
        }
    }
}

void Renderer::AddSkyBoxPass(RenderGraph &graph, SkyBoxPass &sky_box_pass, RGTexture scene_color, RGTexture scene_depth)
{
    const auto sky_map = graph.Import("SkyMap", sky_box_pass.GetSkyMap());
    graph.AddExternalPass("SkyBox", [&sky_box_pass, sky_map, scene_color, scene_depth](RGBuilder &builder) {
        builder.Sampled(sky_map, RHIShaderStageMask::Pixel);
        builder.ColorWrite(scene_color, 0);
        builder.DepthTest(scene_depth);
        return [&sky_box_pass](RGExternalContext &) { sky_box_pass.Render(); };
    });
}

void Renderer::AddToneMappingPass(RenderGraph &graph, RGTexture scene_color, ScreenQuadPass &tone_mapping_pass,
                                  ScreenQuadPass *output_pass, RGTexture screen)
{
    ScreenQuadPass &screen_pass = output_pass ? *output_pass : tone_mapping_pass;
    const auto input = output_pass ? graph.Import("OutputImage", output_pass->GetInput()) : scene_color;
    graph.AddExternalPass(output_pass ? "OutputImage" : "ToneMapping",
                          [&screen_pass, input, screen](RGBuilder &builder) {
                              builder.Sampled(input, RHIShaderStageMask::Pixel);
                              builder.ColorWrite(screen, 0);
                              builder.FullyOverwrites();
                              return [&screen_pass](RGExternalContext &) { screen_pass.Render(); };
                          });
}

void Renderer::AddPresentPasses(RenderGraph &graph, RGTexture screen, UiPass *ui_pass, ScreenQuadPass &present_pass)
{
    const auto back_buffer = graph.Import("BackBuffer", rhi_->GetBackBufferRenderTarget()->GetColorImage(0));

    AddReadback(graph, screen, false);

    if (render_config_.render_ui && ui_pass)
    {
        graph.AddExternalPass("Ui", [ui_pass, screen](RGBuilder &builder) {
            builder.ColorWrite(screen, 0);
            return [ui_pass](RGExternalContext &) { ui_pass->Render(); };
        });

        AddReadback(graph, screen, true);
    }

    graph.AddExternalPass("Present", [&present_pass, screen, back_buffer](RGBuilder &builder) {
        builder.Sampled(screen, RHIShaderStageMask::Pixel);
        builder.ColorWrite(back_buffer, 0);
        builder.FullyOverwrites();
        return [&present_pass](RGExternalContext &) { present_pass.Render(); };
    });
}

void Renderer::ExecuteGraph(RenderGraph &graph)
{
    ASSERT(ThreadManager::IsInRenderThread());

    graph.Compile();

    if (!graph_dump_path_.empty())
    {
        const auto dump = graph.Dump().dump(4);
        const auto saved_path =
            FileManager::GetNativeFileManager()->Write(Path::External(graph_dump_path_), dump.data(), dump.size());
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

    graph.Execute(*rhi_->GetCommandContext());
}
} // namespace sparkle
