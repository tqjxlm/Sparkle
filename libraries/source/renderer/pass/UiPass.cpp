#include "renderer/pass/UiPass.h"

#include "rhi/RHI.h"
#include "rhi/RHIUiHandler.h"

namespace sparkle
{
void UiPass::Render()
{
    rhi_->BeginRenderPass(render_pass_);

    ui_handler_->BeginFrame();

    ui_handler_->Render();

    rhi_->EndRenderPass();
}

void UiPass::InitRenderResources(const RenderConfig &)
{
    RHIRenderPass::Attribute pass_attrib;
    pass_attrib.color_load_op = RHIRenderPass::LoadOp::Load;
    pass_attrib.color_initial_layout = RHIImageLayout::ColorOutput;
    render_pass_ = rhi_->CreateRenderPass(pass_attrib, render_target_, "UiPass");

    ui_handler_ = rhi_->GetUiHandler();
    ui_handler_->Setup(render_pass_);
}
} // namespace sparkle
