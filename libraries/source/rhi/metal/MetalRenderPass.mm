#if FRAMEWORK_APPLE

#include "MetalRenderPass.h"

#include "MetalImage.h"
#include "MetalTimer.h"

namespace sparkle
{
static MTLLoadAction GetMetalLoadAction(RHILoadOp op)
{
    switch (op)
    {
    case RHILoadOp::Load:
        return MTLLoadActionLoad;
    case RHILoadOp::Clear:
        return MTLLoadActionClear;
    case RHILoadOp::None:
        return MTLLoadActionDontCare;
    default:
        UnImplemented(op);
    }
    return MTLLoadActionDontCare;
}

static MTLStoreAction GetMetalStoreAction(RHIStoreOp op)
{
    switch (op)
    {
    case RHIStoreOp::Store:
        return MTLStoreActionStore;
    case RHIStoreOp::None:
        return MTLStoreActionDontCare;
    default:
        UnImplemented(op);
    }
    return MTLStoreActionDontCare;
}

MetalRenderPass::MetalRenderPass(RHIContext *rhi, const Attribute &attribute, const RHIResourceRef<RHIRenderTarget> &rt,
                                 const std::string &name)
    : RHIRenderPass(rhi, attribute, rt, name), descriptor_([MTLRenderPassDescriptor renderPassDescriptor])
{
}

id<MTLRenderCommandEncoder> MetalRenderPass::Begin(id<MTLCommandBuffer> command_buffer) const
{
    const auto &info = GetActiveRenderingInfo();

    FillDescriptor(info);

    if (auto *timer = GetActiveTimer())
    {
        RHICast<MetalTimer>(timer)->AttachTo(descriptor_);
    }

    id<MTLRenderCommandEncoder> render_encoder = [command_buffer renderCommandEncoderWithDescriptor:descriptor_];

    SetDebugInfo(render_encoder, GetName());

    ASSERT_F(render_encoder, "Failed to create render encoder for pass {}", GetName());

    // flip the viewport to map vulkan-convention NDC (y down) to metal (y up).
    // shaders are compiled from vulkan-style slang without a baked-in y-flip.
    auto width = (double)info.width;
    auto height = (double)info.height;
    MTLViewport viewport = {0.0, height, width, -height, 0.0, 1.0};

    [render_encoder setViewport:viewport];

    return render_encoder;
}

MTLRenderPassDescriptor *MetalRenderPass::GetDescriptor() const
{
    FillDescriptor(GetRenderingInfo());
    return descriptor_;
}

void MetalRenderPass::FillDescriptor(const RHIRenderingInfo &info) const
{
    for (auto i = 0u; i < MaxNumColorAttachments; ++i)
    {
        const auto &attachment = info.color_attachments[i];
        auto color_attachment = descriptor_.colorAttachments[i];
        if (!attachment.image)
        {
            color_attachment.texture = nil;
            color_attachment.level = 0;
            color_attachment.slice = 0;
            continue;
        }

        const auto &clear_color = attachment.clear_color;
        color_attachment.texture = RHICast<MetalImage>(attachment.image)->GetResource();
        color_attachment.level = attachment.mip_level;
        color_attachment.slice = attachment.array_layer;
        color_attachment.loadAction = GetMetalLoadAction(attachment.load_op);
        color_attachment.clearColor =
            MTLClearColorMake(clear_color.x(), clear_color.y(), clear_color.z(), clear_color.w());
        color_attachment.storeAction = GetMetalStoreAction(attachment.store_op);
    }

    const auto &depth_attachment = info.depth_attachment;
    if (depth_attachment.image)
    {
        descriptor_.depthAttachment.texture = RHICast<MetalImage>(depth_attachment.image)->GetResource();
        descriptor_.depthAttachment.level = depth_attachment.mip_level;
        descriptor_.depthAttachment.slice = depth_attachment.array_layer;
        descriptor_.depthAttachment.loadAction = GetMetalLoadAction(depth_attachment.load_op);
        descriptor_.depthAttachment.clearDepth = depth_attachment.clear_depth;
        descriptor_.depthAttachment.storeAction = GetMetalStoreAction(depth_attachment.store_op);
    }
    else
    {
        descriptor_.depthAttachment.texture = nil;
        descriptor_.depthAttachment.level = 0;
        descriptor_.depthAttachment.slice = 0;
    }
}
} // namespace sparkle

#endif
