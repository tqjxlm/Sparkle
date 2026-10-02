#include "renderer/pass/UiPass.h"

#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/ColorSlot.h"
#include "rhi/RHI.h"

namespace sparkle
{
UiPass::UiPass(RHIContext *rhi) : ui_handler_(rhi->GetUiHandler())
{
}

void UiPass::AddTo(RenderGraph &graph, RGTexture screen) const
{
    graph.AddRasterPass("Ui", [this, screen](RGBuilder &builder) {
        builder.ColorWrite(screen, ColorSlot::Screen);
        builder.NativeAccess();
        return [this](RGRasterContext &context) {
            auto &command_context = context.GetNativeContext();
            ui_handler_->BeginFrame(command_context.GetRenderingInfo());
            ui_handler_->Render(command_context);
        };
    });
}
} // namespace sparkle
