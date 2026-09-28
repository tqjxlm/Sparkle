#pragma once

#include "renderer/renderer/RasterRenderer.h"

namespace sparkle
{
class ForwardRenderer : public RasterRenderer
{
public:
    ForwardRenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy,
                    RGTexturePool &graph_texture_pool);

    [[nodiscard]] RenderConfig::Pipeline GetRenderMode() const override
    {
        return RenderConfig::Pipeline::Forward;
    }

    ~ForwardRenderer() override;

private:
    void InitScenePasses() override;

    void UpdateScenePasses() override;

    [[nodiscard]] SceneTextures AddScenePasses(RenderGraph &graph, const LightingInputs &lighting) override;

    // render main scene_color
    std::unique_ptr<class ForwardMeshPass> scene_color_pass_;
};
} // namespace sparkle
