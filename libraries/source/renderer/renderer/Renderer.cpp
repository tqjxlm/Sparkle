#include "renderer/renderer/Renderer.h"

#include "core/FileManager.h"
#include "core/Path.h"
#include "core/ThreadManager.h"
#include "renderer/graph/RenderGraph.h"
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

    post_chain_->UpdateFrameData(scene_render_proxy_);

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
    post_chain_->RequestScreenshot(file_path, capture_ui, std::move(on_complete));
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

void Renderer::InitPostChain(PixelFormat screen_format, PostChain::ScreenPass screen_pass)
{
    post_chain_ = std::make_unique<PostChain>(render_config_, rhi_, screen_format, screen_pass);
}

void Renderer::AddPostChain(RenderGraph &graph, RGTexture scene)
{
    post_chain_->AddTo(graph, scene);
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
