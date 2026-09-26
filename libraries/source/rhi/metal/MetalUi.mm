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

void MetalUiHandler::Render(RHICommandContext *command_context)
{
    auto *metal_context = static_cast<MetalCommandContext *>(command_context);

    // it has been set in UiManager::Render()
    auto *draw_data = reinterpret_cast<ImDrawData *>(ImGui::GetIO().UserData);

    ImGui_ImplMetal_RenderDrawData(draw_data, metal_context->GetCommandBuffer(), metal_context->GetRenderEncoder());
}

void MetalUiHandler::BeginFrame()
{
    ImGuiIO &io = ImGui::GetIO();

    // it may be override by platform specific callbacks, so we need to set it every frame
    io.DisplaySize = ImVec2(static_cast<float>(render_pass_->GetRenderTarget()->GetAttribute().width),
                            static_cast<float>(render_pass_->GetRenderTarget()->GetAttribute().height));

    ImGui_ImplMetal_NewFrame(CreateMetalRenderPassDescriptor(render_pass_->GetRenderingInfo()));
}

void MetalUiHandler::Init()
{
    // manually touch resources
    BeginFrame();
}
} // namespace sparkle

#endif
