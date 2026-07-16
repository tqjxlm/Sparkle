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

MetalRenderPass::MetalRenderPass(const Attribute &attribute, const RHIResourceRef<RHIRenderTarget> &rt,
                                 const std::string &name)
    : RHIRenderPass(attribute, rt, name), render_encoder_(nil),
      descriptor_([MTLRenderPassDescriptor renderPassDescriptor])
{
}

void MetalRenderPass::Begin()
{
    auto rhi_rt = render_target_.lock();
    const auto &rt_attribute = rhi_rt->GetAttribute();

    MTLRenderPassDescriptor *descriptor = GetDescriptor();

    render_encoder_ = [context->GetCurrentCommandBuffer() renderCommandEncoderWithDescriptor:descriptor];

    SetDebugInfo(render_encoder_, GetName());

    ASSERT_F(render_encoder_, "Failed to create render encoder for pass {}", GetName());

    // flip the viewport to map vulkan-convention NDC (y down) to metal (y up).
    // shaders are compiled from vulkan-style slang without a baked-in y-flip.
    auto width = (double)(rt_attribute.width >> rt_attribute.mip_level);
    auto height = (double)(rt_attribute.height >> rt_attribute.mip_level);
    MTLViewport viewport = {0.0, height, width, -height, 0.0, 1.0};

    [render_encoder_ setViewport:viewport];
}

void MetalRenderPass::End()
{
    [render_encoder_ endEncoding];
}

MTLRenderPassDescriptor *MetalRenderPass::GetDescriptor() const
{
    auto render_target = render_target_.lock();
    ASSERT(render_target);
    const auto mip_level = render_target->GetAttribute().mip_level;

    for (auto i = 0u; i < RHIRenderTarget::MaxNumColorImage; ++i)
    {
        auto color_image = render_target->GetColorImage(i);
        auto color_attachment = descriptor_.colorAttachments[i];
        if (!color_image)
        {
            color_attachment.texture = nil;
            color_attachment.level = 0;
            continue;
        }

        color_attachment.texture = RHICast<MetalImage>(color_image)->GetResource();
        color_attachment.level = mip_level;
        color_attachment.loadAction = GetMetalLoadAction(attribute_.color_load_op);
        color_attachment.clearColor = MTLClearColorMake(0, 0, 0, 1);
        color_attachment.storeAction = GetMetalStoreAction(attribute_.color_store_op);
    }

    if (render_target->GetDepthImage())
    {
        descriptor_.depthAttachment.texture = RHICast<MetalImage>(render_target->GetDepthImage())->GetResource();
        descriptor_.depthAttachment.level = mip_level;
        descriptor_.depthAttachment.loadAction = GetMetalLoadAction(attribute_.depth_load_op);
        descriptor_.depthAttachment.clearDepth = 1.f;
        descriptor_.depthAttachment.storeAction = GetMetalStoreAction(attribute_.depth_store_op);
    }
    else
    {
        descriptor_.depthAttachment.texture = nil;
        descriptor_.depthAttachment.level = 0;
    }
    return descriptor_;
}
} // namespace sparkle

#endif
