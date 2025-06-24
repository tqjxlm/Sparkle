#include "renderer/pass/ClearTexturePass.h"

#include "rhi/RHI.h"

namespace sparkle
{
ClearTexturePass::ClearTexturePass(RHIContext *ctx, Vector4 clear_value, RHIImageLayout final_layout,
                                   const RHIResourceRef<RHIRenderTarget> &target)
    : PipelinePass(ctx), clear_value_(std::move(clear_value)), final_layout_(final_layout), target_(target)
{
}

void ClearTexturePass::InitRenderResources(const RenderConfig &)
{
    auto render_target = target_.lock();

    RHIRenderPass::Attribute pass_attribute;
    pass_attribute.color_load_op = RHIRenderPass::LoadOp::Clear;
    pass_attribute.color_final_layout = final_layout_;
    pass_attribute.clear_color = clear_value_;
    pass_ = rhi_->CreateRenderPass(pass_attribute, render_target, "ClearTexturePass");
}

void ClearTexturePass::Render()
{
    rhi_->BeginRenderPass(pass_);
    rhi_->EndRenderPass();
}

} // namespace sparkle
