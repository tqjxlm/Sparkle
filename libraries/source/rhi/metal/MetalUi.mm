#if FRAMEWORK_APPLE

#include "MetalUi.h"

#include "MetalContext.h"
#include "MetalRenderPass.h"

#import <imgui_impl_metal.h>

namespace sparkle
{
MetalUiHandler::MetalUiHandler() : RHIUiHandler("MetalUiHandler")
{
    ImGui_ImplMetal_Init(context->GetDevice());

    is_valid_ = true;
}

MetalUiHandler::~MetalUiHandler()
{
    ImGui_ImplMetal_Shutdown();
    is_valid_ = false;
}

void MetalUiHandler::Render()
{
    auto *pass = RHICast<MetalRenderPass>(render_pass_);
    auto encoder = pass->GetRenderEncoder();

    // it has been set in UiManager::Render()
    auto *draw_data = reinterpret_cast<ImDrawData *>(ImGui::GetIO().UserData);

    ImGui_ImplMetal_RenderDrawData(draw_data, context->GetCurrentCommandBuffer(), encoder);
}

void MetalUiHandler::BeginFrame()
{
    auto *pass = RHICast<MetalRenderPass>(render_pass_);

    ImGuiIO &io = ImGui::GetIO();

    // it may be override by platform specific callbacks, so we need to set it every frame
    io.DisplaySize = ImVec2(static_cast<float>(render_pass_->GetRenderTarget()->GetAttribute().width),
                            static_cast<float>(render_pass_->GetRenderTarget()->GetAttribute().height));

    ImGui_ImplMetal_NewFrame(pass->GetDescriptor());
}

void MetalUiHandler::Init()
{
    // manually touch resources
    BeginFrame();
}
} // namespace sparkle

#endif
