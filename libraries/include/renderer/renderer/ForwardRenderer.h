#pragma once

#include "renderer/renderer/Renderer.h"

#include "rhi/RHIImage.h"
#include "rhi/RHIRayTracing.h"
#include "rhi/RHIRenderTarget.h"

namespace sparkle
{
class PrimitiveRenderProxy;

class ForwardRenderer : public Renderer
{
public:
    ForwardRenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy);

    [[nodiscard]] RenderConfig::Pipeline GetRenderMode() const override
    {
        return RenderConfig::Pipeline::forward;
    }

    void Render() override;

    void InitRenderResources() override;

    ~ForwardRenderer() override;

private:
    void Update() override;

    // return true if update is performed
    bool UpdateOutputMode(RenderConfig::OutputImage mode);

    void HandleSceneChanges();

    void RegisterBLAS(PrimitiveRenderProxy *primitive);

    RHIResourceRef<RHIImage> scene_color_;
    RHIResourceRef<RHIImage> scene_depth_;
    RHIResourceRef<RHIImage> screen_color_;

    // render targets
    RHIResourceRef<RHIRenderTarget> screen_color_rt_;

    // generate directional shadow map
    std::unique_ptr<class DepthPass> directional_shadow_pass_;
    // generate scene depth before scene color pass
    std::unique_ptr<class DepthPass> pre_pass_;
    // render main scene_color
    std::unique_ptr<class ForwardMeshPass> scene_color_pass_;
    // skybox to scene_color
    std::unique_ptr<class SkyBoxPass> sky_box_pass_;
    // convert scene_color to screen_color
    std::unique_ptr<class ToneMappingPass> tone_mapping_pass_;
    // for OutputMode other than SceneColor, render to screen_color
    std::unique_ptr<class ScreenQuadPass> texture_output_pass_;
    // render ui elements to screen_color
    std::unique_ptr<class UiPass> ui_pass_;
    // copy screen_color to the backbuffer, probably converting float16 to sRGB
    std::unique_ptr<class ScreenQuadPass> present_pass_;

    // ray tracing resources
    RHIResourceRef<RHITLAS> tlas_;

    std::unique_ptr<class ImageBasedLighting> ibl_;

    RenderConfig::OutputImage output_mode_;

    bool use_ray_tracing_ = false;
    bool use_prepass_ = false;
    bool use_ssao_ = false;
    bool resolve_prepass_depth_ = false;
};
} // namespace sparkle
