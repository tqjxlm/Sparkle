#if ENABLE_VULKAN

#include "VulkanRenderTarget.h"

#include "VulkanContext.h"
#include "VulkanImage.h"
#include "VulkanSwapChain.h"
#include "core/Logger.h"

namespace sparkle
{
void VulkanRenderTarget::Recreate()
{
    if (IsBackBufferTarget())
    {
        ASSERT_EQUAL(attribute_.mip_level, 0);

        SyncWithSwapChain();
    }
    else
    {
        auto mip_layer = attribute_.mip_level;

        image_extent_ = VkExtent2D{attribute_.width >> mip_layer, attribute_.height >> mip_layer};

        for (auto i = 0u; i < RHIRenderTarget::MaxNumColorImage; i++)
        {
            if (color_images_[i])
            {
                color_formats_[i] = GetVkPixelFormat(attribute_.color_attributes_[i].format);
            }
        }
    }

    Log(Debug, "Recreate RT {}: {} * {}, msaa: {}, depth: {}", GetName(), image_extent_.width, image_extent_.height,
        attribute_.msaa_samples, depth_image_ ? depth_image_->GetName() : "none");

    if (attribute_.msaa_samples > 1)
    {
        CreateMsaaResources();
    }

    id_dirty_ = true;
}

void VulkanRenderTarget::SyncWithSwapChain()
{
    ASSERT(IsBackBufferTarget());

    swap_chain_ = context->GetSwapChain();
    swap_chain_->RegisterRenderTarget(this);

    image_extent_ = swap_chain_->GetExtent();

    std::ranges::fill(color_images_, nullptr);

    color_images_[0] = swap_chain_->GetImage(swap_chain_->GetCurrentImageIndex());
    color_formats_[0] = swap_chain_->GetFormat();
}

std::vector<VkImageView> VulkanRenderTarget::GetAttachments(unsigned frame_index) const
{
    std::vector<VkImageView> attachments;

    RHIContext *rhi_context = context->GetRHI();

    for (auto i = 0u; i < RHIRenderTarget::MaxNumColorImage; i++)
    {
        const auto &color_image = color_images_[i];
        if (!color_image)
        {
            continue;
        }

        const RHIImageView *color_view;

        if (IsBackBufferTarget())
        {
            // back buffer image does not have mipmaps
            color_view = swap_chain_->GetImage(frame_index)->GetDefaultView(rhi_context);
        }
        else
        {
            // use mipmap level specified in render target. only ImageViewType::Image2D should be valid here.
            color_view = color_images_[i]->GetView(
                rhi_context, {.base_mip_level = attribute_.mip_level, .base_array_layer = attribute_.array_layer});
        }

        VkImageView output_color_view = RHICast<VulkanImageView>(color_view)->GetView();

        if (attribute_.msaa_samples > 1)
        {
            const auto &msaa_view = msaa_images_[i]->GetView(rhi_context, {.base_mip_level = attribute_.mip_level});
            attachments.push_back(RHICast<VulkanImageView>(msaa_view)->GetView());
        }

        attachments.push_back(output_color_view);
    }

    if (depth_image_)
    {
        attachments.push_back(RHICast<VulkanImageView>(depth_image_->GetDefaultView(rhi_context))->GetView());
    }

    ASSERT_F(!attachments.empty(), "no attachment for render target: {}", GetName());

    return attachments;
}

void VulkanRenderTarget::CreateMsaaResources()
{
    for (auto i = 0u; i < RHIRenderTarget::MaxNumColorImage; i++)
    {
        if (!color_images_[i])
        {
            continue;
        }

        VkFormat const color_format = color_formats_[i];

        RHIImage::Attribute attribute;
        attribute.width = image_extent_.width;
        attribute.height = image_extent_.height;
        attribute.mip_levels = 1;
        attribute.msaa_samples = attribute_.msaa_samples;
        attribute.usages = attribute_.color_attributes_[i].usages | RHIImage::ImageUsage::TransientAttachment;
        attribute.sampler = attribute_.color_attributes_[i].sampler;

        msaa_images_[i] =
            context->GetRHI()->CreateResource<VulkanImage>(attribute, color_format, GetName() + "ColorMsaaRT");
    }
}

VulkanRenderTarget::VulkanRenderTarget(const RHIRenderTarget::Attribute &attribute,
                                       const RHIResourceRef<RHIImage> &depth_image, const std::string &name)
    : RHIRenderTarget(attribute, depth_image, name)
{
    Recreate();
}

VulkanRenderTarget::VulkanRenderTarget(const Attribute &attribute, const ColorImageArray &color_images,
                                       const RHIResourceRef<RHIImage> &depth_image, const std::string &name)
    : RHIRenderTarget(attribute, color_images, depth_image, name)
{
    Recreate();
}

VulkanRenderTarget::~VulkanRenderTarget()
{
    Cleanup();

    if (IsBackBufferTarget())
    {
        swap_chain_ = context->GetSwapChain();
        if (swap_chain_)
        {
            swap_chain_->UnRegisterRenderTarget(this);
        }
    }
}
} // namespace sparkle

#endif
