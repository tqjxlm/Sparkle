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

    // reads the input pixel-locally at ColorSlot::SceneColor where the graph keeps it in the physical pass that wrote
    // it, otherwise samples it
    void SampleInput(RGBuilder &builder, RGTexture input, const RHISampler::SamplerAttribute &sampler) const override;

    [[nodiscard]] const RHIResourceRef<RHIPipelineState> &GetPipeline(const RGRasterContext &context,
                                                                      RGTexture input) const override;

private:
    // of the PIXEL_LOCAL shader variant, compiled at the first frame that reads the input pixel-locally
    mutable RHIResourceRef<RHIPipelineState> pixel_local_pipeline_;
};
} // namespace sparkle
