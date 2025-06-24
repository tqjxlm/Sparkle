#pragma once

#include "renderer/pass/ScreenQuadPass.h"

namespace sparkle
{
class ToneMappingPass : public ScreenQuadPass
{
public:
    ToneMappingPass(RHIContext *ctx, const RHIResourceRef<RHIImage> &input,
                    const RHIResourceRef<RHIRenderTarget> &target)
        : ScreenQuadPass(ctx, input, target)
    {
    }

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

protected:
    void SetupPixelShader() override;

    void BindPixelShaderResources() override;

    void SetupRenderPass() override;
};
} // namespace sparkle
