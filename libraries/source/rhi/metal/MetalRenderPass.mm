#if FRAMEWORK_APPLE

#include "MetalRenderPass.h"

#include "MetalImage.h"

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

MTLRenderPassDescriptor *CreateMetalRenderPassDescriptor(const RHIRenderingInfo &info)
{
    MTLRenderPassDescriptor *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];

    for (auto i = 0u; i < MaxNumColorAttachments; ++i)
    {
        const auto &attachment = info.color_attachments[i];
        if (!attachment.image)
        {
            continue;
        }

        const auto &clear_color = attachment.clear_color;
        auto color_attachment = descriptor.colorAttachments[i];
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
        descriptor.depthAttachment.texture = RHICast<MetalImage>(depth_attachment.image)->GetResource();
        descriptor.depthAttachment.level = depth_attachment.mip_level;
        descriptor.depthAttachment.slice = depth_attachment.array_layer;
        descriptor.depthAttachment.loadAction = GetMetalLoadAction(depth_attachment.load_op);
        descriptor.depthAttachment.clearDepth = depth_attachment.clear_depth;
        descriptor.depthAttachment.storeAction = GetMetalStoreAction(depth_attachment.store_op);
    }

    return descriptor;
}
} // namespace sparkle

#endif
