#if ENABLE_VULKAN

#include "VulkanRenderPass.h"

#include "VulkanContext.h"
#include "VulkanDescriptorSetManager.h"
#include "VulkanImage.h"

namespace sparkle
{
static VkImageView GetAttachmentView(RHIImage *image, unsigned mip_level, unsigned array_layer)
{
    const auto view = image->GetView(context->GetRHI(), {.base_mip_level = mip_level, .base_array_layer = array_layer});
    return RHICast<VulkanImageView>(view)->GetView();
}

// the color output stage also orders the first write to a swap chain image after its acquire semaphore wait
static void TrackAttachmentTransition(std::vector<RHIImageBarrier> &barriers, RHIImage *image, RHIImageLayout layout,
                                      unsigned mip_level, unsigned array_layer, bool discard)
{
    const auto stage = image->GetAttributes().usages & RHIImage::ImageUsage::DepthStencilAttachment
                           ? RHIPipelineStage::LateZ
                           : RHIPipelineStage::ColorOutput;
    auto image_barriers = RHICast<VulkanImage>(image)->TrackTransition({.target_layout = layout,
                                                                        .after_stage = stage,
                                                                        .before_stage = RHIPipelineStage::Bottom,
                                                                        .base_mip = mip_level,
                                                                        .mip_count = 1,
                                                                        .base_array_layer = array_layer,
                                                                        .array_layer_count = 1,
                                                                        .discard = discard});
    barriers.insert(barriers.end(), image_barriers.begin(), image_barriers.end());
}

void VulkanRenderPass::Begin(VulkanCommandContext &command_context)
{
    context->GetDescriptorSetManager().UpdateDirtyResourceArrays();

    const auto &info = GetActiveRenderingInfo();

    std::vector<RHIImageBarrier> barriers;

    std::array<VkRenderingAttachmentInfo, MaxNumColorAttachments> color_infos{};
    uint32_t color_attachment_count = 0;
    for (auto slot = 0u; slot < MaxNumColorAttachments; slot++)
    {
        const auto &attachment = info.color_attachments[slot];
        auto &color_info = color_infos[slot];
        color_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!attachment.image)
        {
            continue;
        }

        color_attachment_count = slot + 1;

        const bool discard = attachment.load_op != RHILoadOp::Load;
        if (attachment.resolve_image)
        {
            TrackAttachmentTransition(barriers, attachment.image, RHIImageLayout::ColorOutput, 0, 0, discard);
            TrackAttachmentTransition(barriers, attachment.resolve_image, RHIImageLayout::ColorOutput,
                                      attachment.mip_level, attachment.array_layer, true);

            color_info.imageView = GetAttachmentView(attachment.image, 0, 0);
            color_info.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color_info.resolveImageView =
                GetAttachmentView(attachment.resolve_image, attachment.mip_level, attachment.array_layer);
            color_info.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        else
        {
            TrackAttachmentTransition(barriers, attachment.image, RHIImageLayout::ColorOutput, attachment.mip_level,
                                      attachment.array_layer, discard);

            color_info.imageView = GetAttachmentView(attachment.image, attachment.mip_level, attachment.array_layer);
        }

        color_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_info.loadOp = GetAttachmentLoadOp(attachment.load_op);
        color_info.storeOp = GetAttachmentStoreOp(attachment.store_op);
        color_info.clearValue.color = {{attachment.clear_color.x(), attachment.clear_color.y(),
                                        attachment.clear_color.z(), attachment.clear_color.w()}};
    }

    const auto &depth_attachment = info.depth_attachment;
    VkRenderingAttachmentInfo depth_info{};
    depth_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (depth_attachment.image)
    {
        TrackAttachmentTransition(barriers, depth_attachment.image, RHIImageLayout::DepthStencilOutput,
                                  depth_attachment.mip_level, depth_attachment.array_layer,
                                  depth_attachment.load_op != RHILoadOp::Load);

        depth_info.imageView =
            GetAttachmentView(depth_attachment.image, depth_attachment.mip_level, depth_attachment.array_layer);
        depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_info.loadOp = GetAttachmentLoadOp(depth_attachment.load_op);
        depth_info.storeOp = GetAttachmentStoreOp(depth_attachment.store_op);
        depth_info.clearValue.depthStencil = {.depth = depth_attachment.clear_depth, .stencil = 0};
    }

    command_context.Barrier(barriers, {});

    const VkRect2D render_area{.offset = {.x = 0, .y = 0}, .extent = {.width = info.width, .height = info.height}};

    VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea = render_area;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = color_attachment_count;
    rendering_info.pColorAttachments = color_infos.data();
    rendering_info.pDepthAttachment = depth_attachment.image ? &depth_info : nullptr;

    vkCmdBeginRendering(command_context.GetCommandBuffer(), &rendering_info);

    const VkViewport viewport{.x = 0.0f,
                              .y = 0.0f,
                              .width = static_cast<float>(info.width),
                              .height = static_cast<float>(info.height),
                              .minDepth = 0.0f,
                              .maxDepth = 1.0f};
    command_context.SetViewportAndScissor(viewport, render_area);
}

void VulkanRenderPass::End(VulkanCommandContext &command_context)
{
    vkCmdEndRendering(command_context.GetCommandBuffer());

    const auto &info = GetActiveRenderingInfo();

    std::vector<RHIImageBarrier> barriers;

    for (const auto &attachment : info.color_attachments)
    {
        auto *image = attachment.resolve_image ? attachment.resolve_image : attachment.image;
        if (image && attachment.final_layout != RHIImageLayout::ColorOutput)
        {
            TrackAttachmentTransition(barriers, image, attachment.final_layout, attachment.mip_level,
                                      attachment.array_layer, false);
        }
    }

    const auto &depth_attachment = info.depth_attachment;
    if (depth_attachment.image && depth_attachment.final_layout != RHIImageLayout::DepthStencilOutput)
    {
        TrackAttachmentTransition(barriers, depth_attachment.image, depth_attachment.final_layout,
                                  depth_attachment.mip_level, depth_attachment.array_layer, false);
    }

    command_context.Barrier(barriers, {});
}
} // namespace sparkle

#endif
