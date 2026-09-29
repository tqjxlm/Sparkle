#pragma once

#include "renderer/pass/ScreenQuadPass.h"

namespace sparkle
{
class ToneMappingPass : public ScreenQuadPass
{
public:
    // the format of the screen of the renderers that tone map on the GPU
    static constexpr PixelFormat ScreenFormat = PixelFormat::B8G8R8A8Srgb;

    ToneMappingPass(RHIContext *ctx, PixelFormat output_format)
        : ScreenQuadPass(ctx, "ToneMapping", output_format, InputFilter::Bilinear)
    {
    }

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

protected:
    void SetupPixelShader() override;

    void BindPixelShaderResources() override;

    void SampleInput(RGBuilder &builder, RGTexture input, const RHISampler::SamplerAttribute &sampler) const override;
};
} // namespace sparkle
