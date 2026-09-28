#include "renderer/renderer/ForwardRenderer.h"

#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/ForwardMeshPass.h"

namespace sparkle
{
ForwardRenderer::ForwardRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                 SceneRenderProxy *scene_render_proxy, RGTexturePool &graph_texture_pool)
    : RasterRenderer(render_config, rhi_context, scene_render_proxy, graph_texture_pool)
{
    ASSERT_EQUAL(render_config_.pipeline, RenderConfig::Pipeline::Forward);
}

void ForwardRenderer::InitScenePasses()
{
    scene_color_pass_ = PipelinePass::Create<ForwardMeshPass>(render_config_, rhi_, scene_render_proxy_,
                                                              SceneColorDesc.format, SceneDepthDesc.format);
}

void ForwardRenderer::UpdateScenePasses()
{
    scene_color_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
}

RasterRenderer::SceneTextures ForwardRenderer::AddScenePasses(RenderGraph &graph, const LightingInputs &lighting)
{
    const auto scene_color = graph.CreateTexture("SceneColor", SceneColorDesc);
    const auto scene_depth = graph.CreateTexture("SceneDepth", SceneDepthDesc);
    scene_color_pass_->AddTo(graph, lighting, scene_color, scene_depth);
    return {.color = scene_color, .depth = scene_depth};
}

ForwardRenderer::~ForwardRenderer() = default;

} // namespace sparkle
