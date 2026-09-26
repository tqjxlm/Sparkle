#include "rhi/RHIRenderPass.h"

namespace sparkle
{
// the color output stage also orders the first write to a swap chain image after its acquire semaphore wait
static void TrackAttachmentTransition(std::vector<RHIImageBarrier> &barriers, RHIImage *image, RHIImageLayout layout,
                                      unsigned mip_level, unsigned array_layer, bool discard)
{
    const auto stage = image->GetAttributes().usages & RHIImage::ImageUsage::DepthStencilAttachment
                           ? RHIPipelineStage::LateZ
                           : RHIPipelineStage::ColorOutput;
    auto image_barriers = image->TrackTransition({.target_layout = layout,
                                                  .after_stage = stage,
                                                  .before_stage = RHIPipelineStage::Bottom,
                                                  .base_mip = mip_level,
                                                  .mip_count = 1,
                                                  .base_array_layer = array_layer,
                                                  .array_layer_count = 1,
                                                  .discard = discard});
    barriers.insert(barriers.end(), image_barriers.begin(), image_barriers.end());
}

RHIRenderingInfo RHIRenderPass::GetRenderingInfo() const
{
    const auto *render_target = GetRenderTarget();
    ASSERT_F(render_target, "render pass {} has no render target", GetName());
    const auto &rt_attribute = render_target->GetAttribute();

    RHIRenderingInfo info;
    info.width = rt_attribute.width >> rt_attribute.mip_level;
    info.height = rt_attribute.height >> rt_attribute.mip_level;
    info.samples = rt_attribute.msaa_samples;

    for (auto slot = 0u; slot < MaxNumColorAttachments; slot++)
    {
        const auto &color_image = render_target->GetColorImages()[slot];
        if (!color_image)
        {
            continue;
        }

        const auto &msaa_image = render_target->GetMsaaImage(slot);

        auto &attachment = info.color_attachments[slot];
        attachment.image = msaa_image ? msaa_image.get() : color_image.get();
        attachment.resolve_image = msaa_image ? color_image.get() : nullptr;
        attachment.mip_level = rt_attribute.mip_level;
        attachment.array_layer = rt_attribute.array_layer;
        attachment.load_op = attribute_.color_load_op;
        attachment.store_op = attribute_.color_store_op;
        attachment.clear_color = attribute_.clear_color;
    }

    if (const auto &depth_image = render_target->GetDepthImage())
    {
        auto &attachment = info.depth_attachment;
        attachment.image = depth_image.get();
        attachment.mip_level = rt_attribute.mip_level;
        attachment.array_layer = rt_attribute.array_layer;
        attachment.load_op = attribute_.depth_load_op;
        attachment.store_op = attribute_.depth_store_op;
    }

    return info;
}

std::vector<RHIImageBarrier> RHIRenderPass::TrackBeginTransitions(const RHIRenderingInfo &info)
{
    std::vector<RHIImageBarrier> barriers;

    for (const auto &attachment : info.color_attachments)
    {
        if (!attachment.image)
        {
            continue;
        }

        const bool discard = attachment.load_op != RHILoadOp::Load;
        if (attachment.resolve_image)
        {
            TrackAttachmentTransition(barriers, attachment.image, RHIImageLayout::ColorOutput, 0, 0, discard);
            TrackAttachmentTransition(barriers, attachment.resolve_image, RHIImageLayout::ColorOutput,
                                      attachment.mip_level, attachment.array_layer, true);
        }
        else
        {
            TrackAttachmentTransition(barriers, attachment.image, RHIImageLayout::ColorOutput, attachment.mip_level,
                                      attachment.array_layer, discard);
        }
    }

    const auto &depth_attachment = info.depth_attachment;
    if (depth_attachment.image)
    {
        TrackAttachmentTransition(barriers, depth_attachment.image, RHIImageLayout::DepthStencilOutput,
                                  depth_attachment.mip_level, depth_attachment.array_layer,
                                  depth_attachment.load_op != RHILoadOp::Load);
    }

    return barriers;
}

std::vector<RHIImageBarrier> RHIRenderPass::TrackEndTransitions(const RHIRenderingInfo &info) const
{
    std::vector<RHIImageBarrier> barriers;

    if (attribute_.color_final_layout != RHIImageLayout::ColorOutput)
    {
        for (const auto &attachment : info.color_attachments)
        {
            if (auto *image = attachment.resolve_image ? attachment.resolve_image : attachment.image)
            {
                TrackAttachmentTransition(barriers, image, attribute_.color_final_layout, attachment.mip_level,
                                          attachment.array_layer, false);
            }
        }
    }

    const auto &depth_attachment = info.depth_attachment;
    if (depth_attachment.image && attribute_.depth_final_layout != RHIImageLayout::DepthStencilOutput)
    {
        TrackAttachmentTransition(barriers, depth_attachment.image, attribute_.depth_final_layout,
                                  depth_attachment.mip_level, depth_attachment.array_layer, false);
    }

    return barriers;
}
} // namespace sparkle
