#pragma once

#include "renderer/pass/ScreenQuadPass.h"

namespace sparkle
{
class ToneMappingPass : public ScreenQuadPass
{
public:
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
