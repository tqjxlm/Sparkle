#include "rhi/RHIRenderPass.h"

namespace sparkle
{
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
        attachment.final_layout = attribute_.color_final_layout;
    }

    if (const auto &depth_image = render_target->GetDepthImage())
    {
        auto &attachment = info.depth_attachment;
        attachment.image = depth_image.get();
        attachment.mip_level = rt_attribute.mip_level;
        attachment.array_layer = rt_attribute.array_layer;
        attachment.load_op = attribute_.depth_load_op;
        attachment.store_op = attribute_.depth_store_op;
        attachment.final_layout = attribute_.depth_final_layout;
    }

    return info;
}
} // namespace sparkle
