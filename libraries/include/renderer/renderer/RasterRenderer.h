#pragma once

#include "renderer/renderer/Renderer.h"

namespace sparkle
{
class DepthPass;
class ImageBasedLighting;
class SkyBoxPass;
class SkyRenderProxy;
struct LightingInputs;

// a renderer that rasterizes the scene: the IBL cook while a map cooks, a directional shadow map, the scene passes
// the derived renderer adds, and the sky box over the scene they leave, followed by the tone mapping post chain
class RasterRenderer : public Renderer
{
public:
    void InitRenderResources() final;

    ~RasterRenderer() override;

protected:
    RasterRenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy,
                   RGTexturePool &graph_texture_pool);

    static constexpr RGTextureDesc SceneDepthDesc{.format = PixelFormat::D32, .size_class = RGSizeClass::Scene};

    static constexpr RGTextureDesc SceneColorDesc{.format = PixelFormat::RGBAFloat16, .size_class = RGSizeClass::Scene};

    // the textures the scene passes leave the scene in, of SceneColorDesc and SceneDepthDesc
    struct SceneTextures
    {
        RGTexture color;
        RGTexture depth;
    };

private:
    virtual void InitScenePasses() = 0;

    virtual void UpdateScenePasses() = 0;

    // adds the passes that draw the scene lit by `lighting`
    [[nodiscard]] virtual SceneTextures AddScenePasses(RenderGraph &graph, const LightingInputs &lighting) = 0;

    void Update() final;

    [[nodiscard]] RGTexture BuildGraph(RenderGraph &graph) final;

    void HandleSceneChanges();

    // null without a directional light
    std::unique_ptr<DepthPass> directional_shadow_pass_;
    std::unique_ptr<SkyBoxPass> sky_box_pass_;

    ImageBasedLighting *ibl_ = nullptr;

    SkyRenderProxy *bound_sky_proxy_ = nullptr;

    bool ibl_cook_pending_ = false;
};
} // namespace sparkle
