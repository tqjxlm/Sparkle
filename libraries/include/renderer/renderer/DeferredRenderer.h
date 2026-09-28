#pragma once

#include "renderer/renderer/Renderer.h"

namespace sparkle
{
class SkyRenderProxy;

class DeferredRenderer : public Renderer
{
public:
    DeferredRenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy,
                     RGTexturePool &graph_texture_pool);

    [[nodiscard]] RenderConfig::Pipeline GetRenderMode() const override
    {
        return RenderConfig::Pipeline::Deferred;
    }

    void Render() override;

    void InitRenderResources() override;

    ~DeferredRenderer() override;

private:
    void Update() override;

    void HandleSceneChanges();

    std::unique_ptr<class DepthPass> directional_shadow_pass_;
    std::unique_ptr<class GBufferPass> gbuffer_pass_;
    std::unique_ptr<class DirectionalLightingPass> directional_lighting_pass_;
    std::unique_ptr<class SkyBoxPass> sky_box_pass_;

    // convert scene_color to the screen
    std::unique_ptr<class ToneMappingPass> tone_mapping_pass_;

    class ImageBasedLighting *ibl_ = nullptr;

    SkyRenderProxy *bound_sky_proxy_ = nullptr;

    bool ibl_cook_pending_ = false;
};
} // namespace sparkle
