#pragma once

#include "renderer/renderer/RasterRenderer.h"

namespace sparkle
{
class DeferredRenderer : public RasterRenderer
{
public:
    DeferredRenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy,
                     RGTexturePool &graph_texture_pool);

    [[nodiscard]] RenderConfig::Pipeline GetRenderMode() const override
    {
        return RenderConfig::Pipeline::Deferred;
    }

    ~DeferredRenderer() override;

private:
    void InitScenePasses() override;

    void UpdateScenePasses() override;

    [[nodiscard]] SceneTextures AddScenePasses(RenderGraph &graph, const LightingInputs &lighting) override;

    std::unique_ptr<class GBufferPass> gbuffer_pass_;
    std::unique_ptr<class DirectionalLightingPass> directional_lighting_pass_;
};
} // namespace sparkle
