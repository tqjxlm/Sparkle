#pragma once

#include "renderer/renderer/Renderer.h"

#include "renderer/resource/GBuffer.h"

namespace sparkle
{
class DeferredRenderer : public Renderer
{
public:
    DeferredRenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy);

    [[nodiscard]] RenderConfig::Pipeline GetRenderMode() const override
    {
        return RenderConfig::Pipeline::deferred;
    }

    void Render() override;

    void InitRenderResources() override;

    ~DeferredRenderer() override;

private:
    void Update() override;

    // return true if update is performed
    bool UpdateOutputMode(RenderConfig::OutputImage mode);

    void HandleSceneChanges();

    RHIResourceRef<RHIImage> scene_color_;
    RHIResourceRef<RHIImage> scene_depth_;
    RHIResourceRef<RHIImage> screen_color_;

    RHIResourceRef<RHIRenderTarget> screen_color_rt_;
    RHIResourceRef<RHIRenderTarget> lighting_rt_;

    GBuffer gbuffer_;

    std::unique_ptr<class DepthPass> directional_shadow_pass_;
    std::unique_ptr<class GBufferPass> gbuffer_pass_;
    std::unique_ptr<class DirectionalLightingPass> directional_lighting_pass_;
    std::unique_ptr<class SkyBoxPass> sky_box_pass_;

    // convert scene_color to screen_color
    std::unique_ptr<class ToneMappingPass> tone_mapping_pass_;
    // for OutputMode other than SceneColor, render to screen_color
    std::unique_ptr<class ScreenQuadPass> debug_output_pass_;
    // render ui elements to screen_color
    std::unique_ptr<class UiPass> ui_pass_;
    // copy screen_color to the backbuffer, probably converting float16 to sRGB
    std::unique_ptr<class ScreenQuadPass> present_pass_;

    std::unique_ptr<class ImageBasedLighting> ibl_;

    RenderConfig::OutputImage output_mode_;
};
} // namespace sparkle
