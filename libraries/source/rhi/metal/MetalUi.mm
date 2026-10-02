#if FRAMEWORK_APPLE

#include "MetalUi.h"

#include "MetalContext.h"
#include "MetalRenderPass.h"

#import <imgui_impl_metal.h>

namespace sparkle
{
MetalUiHandler::MetalUiHandler()
    : RHIUiHandler("MetalUiHandler", ImGui_ImplMetal_UpdateTexture, context->GetRHI()->GetMaxFramesInFlight())
{
    ImGui_ImplMetal_Init(context->GetDevice());

    is_valid_ = true;
}

MetalUiHandler::~MetalUiHandler()
{
    ShutdownTextureQueue();
    ImGui_ImplMetal_Shutdown();
    is_valid_ = false;
}

void MetalUiHandler::Render(RHICommandContext &command_context)
{
    auto &metal_context = static_cast<MetalCommandContext &>(command_context);

    // it has been set in UiManager::Render()
    auto *draw_data = reinterpret_cast<ImDrawData *>(ImGui::GetIO().UserData);

    ProcessTextureRequests(*draw_data);

    ImGui_ImplMetal_RenderDrawData(draw_data, metal_context.GetCommandBuffer(), metal_context.GetRenderEncoder());
}

void MetalUiHandler::BeginFrame(const RHIRenderingInfo &info)
{
    ImGui_ImplMetal_NewFrame(CreateMetalRenderPassDescriptor(info));
}
} // namespace sparkle

#endif
