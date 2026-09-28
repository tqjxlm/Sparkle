#pragma once

#include "renderer/pass/ScreenQuadPass.h"

namespace sparkle
{
struct LightingInputs;

// lights the surfaces of a GBuffer with the directional light and the sky light
class DirectionalLightingPass : public ScreenQuadPass
{
public:
    // draws into a color attachment of `output_format` at slot 0
    DirectionalLightingPass(RHIContext *ctx, PixelFormat output_format)
        : ScreenQuadPass(ctx, "Lighting", output_format, InputFilter::Nearest)
    {
    }

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

    // adds a Raster pass lighting the surfaces of `gbuffer` at the depths of `scene_depth`, both sampled, into all of
    // `scene_color`
    void AddTo(RenderGraph &graph, const LightingInputs &lighting, RGTexture gbuffer, RGTexture scene_depth,
               RGTexture scene_color) const;

protected:
    void SetupPixelShader() override;

    void BindPixelShaderResources() override;
};
} // namespace sparkle
