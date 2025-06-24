#if FRAMEWORK_APPLE

#include "MetalRenderPass.h"

#include "MetalContext.h"
#include "MetalImage.h"

namespace sparkle
{
static MTLLoadAction GetMetalLoadAction(RHIRenderPass::LoadOp op)
{
    switch (op)
    {
    case RHIRenderPass::LoadOp::Load:
        return MTLLoadActionLoad;
    case RHIRenderPass::LoadOp::Clear:
        return MTLLoadActionClear;
    case RHIRenderPass::LoadOp::None:
        return MTLLoadActionDontCare;
    default:
        UnImplemented(op);
    }
    return MTLLoadActionDontCare;
}

static MTLStoreAction GetMetalStoreAction(RHIRenderPass::StoreOp op)
{
    switch (op)
    {
    case RHIRenderPass::StoreOp::Store:
        return MTLStoreActionStore;
    case RHIRenderPass::StoreOp::None:
        return MTLStoreActionDontCare;
    default:
        UnImplemented(op);
    }
    return MTLStoreActionDontCare;
}

void MetalRenderPass::Begin()
{
    auto rhi_rt = render_target_.lock();
    const auto &rt_attribute = rhi_rt->GetAttribute();

    MTLRenderPassDescriptor *descriptor = GetDescriptor();

    render_encoder_ = [context->GetCurrentCommandBuffer() renderCommandEncoderWithDescriptor:descriptor];

    SetDebugInfo(render_encoder_, GetName());

    ASSERT_F(render_encoder_, "Failed to create render encoder for pass {}", GetName());

    MTLViewport viewport = {0.0,
                            0.0,
                            (float)(rt_attribute.width >> rt_attribute.mip_level),
                            (float)(rt_attribute.height >> rt_attribute.mip_level),
                            0.0,
                            1.0};

    [render_encoder_ setViewport:viewport];
}

void MetalRenderPass::End()
{
    [render_encoder_ endEncoding];
}

MTLRenderPassDescriptor *MetalRenderPass::GetDescriptor() const
{
    // TODO(tqjxlm): cache this

    MTLRenderPassDescriptor *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];

    auto render_target = render_target_.lock();

    for (auto i = 0u; i < RHIRenderTarget::MaxNumColorImage; ++i)
    {
        if (!render_target->GetColorImage(i))
        {
            continue;
        }

        auto color_attachment = descriptor.colorAttachments[i];
        color_attachment.texture = RHICast<MetalImage>(GetRenderTarget()->GetColorImage(i))->GetResource();
        color_attachment.level = GetRenderTarget()->GetAttribute().mip_level;
        color_attachment.loadAction = GetMetalLoadAction(attribute_.color_load_op);
        color_attachment.clearColor = MTLClearColorMake(0, 0, 0, 1);
        color_attachment.storeAction = GetMetalStoreAction(attribute_.color_store_op);
    }

    if (render_target->GetDepthImage())
    {
        descriptor.depthAttachment.texture = RHICast<MetalImage>(GetRenderTarget()->GetDepthImage())->GetResource();
        descriptor.depthAttachment.level = GetRenderTarget()->GetAttribute().mip_level;
        descriptor.depthAttachment.loadAction = GetMetalLoadAction(attribute_.depth_load_op);
        descriptor.depthAttachment.clearDepth = 1.f;
        descriptor.depthAttachment.storeAction = GetMetalStoreAction(attribute_.depth_store_op);
    }
    return descriptor;
}
} // namespace sparkle

#endif
