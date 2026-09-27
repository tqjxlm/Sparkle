#include "renderer/pass/UiPass.h"

#include "renderer/graph/RenderGraph.h"
#include "rhi/RHI.h"

namespace sparkle
{
UiPass::UiPass(RHIContext *rhi, PixelFormat screen_format) : ui_handler_(rhi->GetUiHandler())
{
    RHIAttachmentSignature signature;
    signature.color_formats[0] = screen_format;
    ui_handler_->Setup(signature);
}

void UiPass::AddTo(RenderGraph &graph, RGTexture screen) const
{
    graph.AddRasterPass("Ui", [this, screen](RGBuilder &builder) {
        builder.ColorWrite(screen, 0);
        builder.NativeAccess();
        return [this](RGRasterContext &context) {
            auto &command_context = context.GetNativeContext();
            ui_handler_->BeginFrame(command_context.GetRenderingInfo());
            ui_handler_->Render(&command_context);
        };
    });
}
} // namespace sparkle
