#include "renderer/renderer/DeferredRenderer.h"

#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/DirectionalLightingPass.h"
#include "renderer/pass/GBufferPass.h"
#include "renderer/pass/PipelinePass.h"

namespace sparkle
{
DeferredRenderer::DeferredRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                   SceneRenderProxy *scene_render_proxy, RGTexturePool &graph_texture_pool)
    : RasterRenderer(render_config, rhi_context, scene_render_proxy, graph_texture_pool)
{
    ASSERT_EQUAL(render_config_.pipeline, RenderConfig::Pipeline::Deferred);
}

DeferredRenderer::~DeferredRenderer() = default;

void DeferredRenderer::InitScenePasses()
{
    gbuffer_pass_ = PipelinePass::Create<GBufferPass>(render_config_, rhi_, scene_render_proxy_, SceneDepthDesc.format);

    directional_lighting_pass_ =
        PipelinePass::Create<DirectionalLightingPass>(render_config_, rhi_, SceneColorDesc.format);
}

void DeferredRenderer::UpdateScenePasses()
{
    gbuffer_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    directional_lighting_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
}

RasterRenderer::SceneTextures DeferredRenderer::AddScenePasses(RenderGraph &graph, const LightingInputs &lighting)
{
    const auto scene_depth = graph.CreateTexture("SceneDepth", SceneDepthDesc);
    const auto gbuffer = gbuffer_pass_->AddTo(graph, scene_depth);
    const auto scene_color = graph.CreateTexture("SceneColor", SceneColorDesc);
    directional_lighting_pass_->AddTo(graph, lighting, gbuffer, scene_depth, scene_color);
    return {.color = scene_color, .depth = scene_depth};
}
} // namespace sparkle
