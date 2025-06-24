#include "renderer/renderer/Renderer.h"

#include "core/ThreadManager.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/renderer/CPURenderer.h"
#include "renderer/renderer/DeferredRenderer.h"
#include "renderer/renderer/ForwardRenderer.h"
#include "renderer/renderer/GPURenderer.h"
#include "rhi/RHI.h"

namespace sparkle
{
std::unique_ptr<Renderer> Renderer::CreateRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                                   SceneRenderProxy *scene_render_proxy)
{
    ASSERT(ThreadManager::IsInRenderThread());

    std::unique_ptr<Renderer> renderer;

    switch (render_config.pipeline)
    {
    case RenderConfig::Pipeline::cpu:
        renderer = std::make_unique<CPURenderer>(render_config, rhi_context, scene_render_proxy);
        break;
    case RenderConfig::Pipeline::gpu:
        renderer = std::make_unique<GPURenderer>(render_config, rhi_context, scene_render_proxy);
        break;
    case RenderConfig::Pipeline::forward:
        renderer = std::make_unique<ForwardRenderer>(render_config, rhi_context, scene_render_proxy);
        break;
    case RenderConfig::Pipeline::deferred:
        renderer = std::make_unique<DeferredRenderer>(render_config, rhi_context, scene_render_proxy);
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
    : rhi_(rhi_context), scene_render_proxy_(scene_render_proxy),
      image_size_{render_config.image_width, render_config.image_height}, render_config_(render_config)
{
    Log(Info, "View size [{}, {}]", image_size_.x(), image_size_.y());
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
} // namespace sparkle
