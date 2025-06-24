#pragma once

#include "renderer/pass/PipelinePass.h"

#include "rhi/RHIRenderPass.h"
#include "rhi/RHIRenderTarget.h"
#include "rhi/RHIUiHandler.h"

namespace sparkle
{
class UiPass : public PipelinePass
{
public:
    explicit UiPass(RHIContext *rhi, const RHIResourceRef<RHIRenderTarget> &render_target)
        : PipelinePass(rhi), render_target_(render_target)
    {
    }

    void Render() override;

    void InitRenderResources(const RenderConfig &config) override;

    void UpdateFrameData(const RenderConfig & /*config*/, SceneRenderProxy * /*scene*/) override
    {
    }

private:
    RHIResourceRef<RHIUiHandler> ui_handler_;
    RHIResourceRef<RHIRenderPass> render_pass_;
    RHIResourceRef<RHIRenderTarget> render_target_;
};
} // namespace sparkle
