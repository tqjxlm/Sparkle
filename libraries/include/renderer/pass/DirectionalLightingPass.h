#pragma once

#include "renderer/pass/ColorSlot.h"
#include "renderer/pass/ScreenQuadPass.h"

namespace sparkle
{
struct LightingInputs;

// lights the surfaces of a GBuffer with the directional light and the sky light
class DirectionalLightingPass : public ScreenQuadPass
{
public:
    // draws into a color attachment of `output_format` at ColorSlot::SceneColor
    DirectionalLightingPass(RHIContext *ctx, PixelFormat output_format)
        : ScreenQuadPass(ctx, "Lighting", output_format, ColorSlot::SceneColor, InputFilter::Nearest)
    {
    }

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

    // adds a Raster pass lighting the surfaces of `gbuffer` at the depths of `depth_copy` into all of `scene_color`.
    // it reads both at its pixel: pixel-locally at ColorSlot::GBufferPacked and ColorSlot::DepthCopy where the graph
    // keeps them in the physical pass that wrote them, otherwise loaded.
    void AddTo(RenderGraph &graph, const LightingInputs &lighting, RGTexture gbuffer, RGTexture depth_copy,
               RGTexture scene_color) const;

protected:
    void SetupPixelShader() override;

    void BindPixelShaderResources() override;

    [[nodiscard]] const RHIResourceRef<RHIPipelineState> &GetPipeline(const RGRasterContext &context,
                                                                      RGTexture gbuffer) const override;

private:
    void BindView(RHIPipelineState &pipeline) const;

    RHIResourceRef<RHIBuffer> view_buffer_;

    // of the PIXEL_LOCAL shader variant, compiled at the first frame that reads the GBuffer pixel-locally
    mutable RHIResourceRef<RHIPipelineState> pixel_local_pipeline_;
};
} // namespace sparkle
