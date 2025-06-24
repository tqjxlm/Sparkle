#pragma once

#include "renderer/pass/PipelinePass.h"

#include "core/math/Types.h"
#include "rhi/RHIRenderPass.h"
#include "rhi/RHIRenderTarget.h"

namespace sparkle
{
class ClearTexturePass : public PipelinePass
{
public:
    ClearTexturePass(RHIContext *ctx, Vector4 clear_value, RHIImageLayout final_layout,
                     const RHIResourceRef<RHIRenderTarget> &target);

    void InitRenderResources(const RenderConfig &config) override;
    void Render() override;

protected:
    Vector4 clear_value_;
    RHIImageLayout final_layout_;
    RHIResourceRef<RHIRenderPass> pass_;
    RHIResourceWeakRef<RHIRenderTarget> target_;
};
} // namespace sparkle
