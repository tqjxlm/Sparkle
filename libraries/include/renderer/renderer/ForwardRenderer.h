#pragma once

#include "renderer/renderer/Renderer.h"

namespace sparkle
{
class SkyRenderProxy;

class ForwardRenderer : public Renderer
{
public:
    ForwardRenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy,
                    RGTexturePool &graph_texture_pool);

    [[nodiscard]] RenderConfig::Pipeline GetRenderMode() const override
    {
        return RenderConfig::Pipeline::Forward;
    }

    void Render() override;

    void InitRenderResources() override;

    ~ForwardRenderer() override;

private:
    void Update() override;

    void HandleSceneChanges();

    // generate directional shadow map
    std::unique_ptr<class DepthPass> directional_shadow_pass_;
    // render main scene_color
    std::unique_ptr<class ForwardMeshPass> scene_color_pass_;
    // skybox to scene_color
    std::unique_ptr<class SkyBoxPass> sky_box_pass_;
    // convert scene_color to the screen
    std::unique_ptr<class ToneMappingPass> tone_mapping_pass_;
    // shows an output mode's image on the screen in place of the tone-mapped scene
    std::unique_ptr<class ScreenQuadPass> texture_output_pass_;

    class ImageBasedLighting *ibl_ = nullptr;

    SkyRenderProxy *bound_sky_proxy_ = nullptr;

    bool ibl_cook_pending_ = false;
};
} // namespace sparkle
