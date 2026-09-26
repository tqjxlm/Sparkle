#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#include "VulkanImage.h"

namespace sparkle
{
inline VkAttachmentLoadOp GetAttachmentLoadOp(RHILoadOp op)
{
    switch (op)
    {
    case RHILoadOp::None:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    case RHILoadOp::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case RHILoadOp::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    default:
        UnImplemented(op);
    }
}

inline VkAttachmentStoreOp GetAttachmentStoreOp(RHIStoreOp op)
{
    switch (op)
    {
    case RHIStoreOp::None:
        return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    case RHIStoreOp::Store:
        return VK_ATTACHMENT_STORE_OP_STORE;
    default:
        UnImplemented(op);
    }
}

inline VkFormat FindSupportedFormat(const std::vector<VkFormat> &candidates, VkImageTiling tiling,
                                    VkFormatFeatureFlags features, VkPhysicalDevice physicalDevice)
{
    for (VkFormat const format : candidates)
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

        if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
        {
            return format;
        }
        if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features)
        {
            return format;
        }
    }

    ASSERT_F(false, "failed to find supported format!");
    return VK_FORMAT_UNDEFINED;
}

inline VkFormat FindDepthFormat(VkPhysicalDevice physicalDevice)
{
    return FindSupportedFormat({VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                               VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, physicalDevice);
}

// color_formats backs the returned struct and must outlive it
inline VkPipelineRenderingCreateInfo GetVkPipelineRenderingCreateInfo(
    const RHIAttachmentSignature &signature, std::array<VkFormat, MaxNumColorAttachments> &color_formats)
{
    // PixelFormat::Count marks an unused attachment
    auto get_format = [](PixelFormat format) {
        return format == PixelFormat::Count ? VK_FORMAT_UNDEFINED : GetVkPixelFormat(format);
    };

    uint32_t color_attachment_count = 0;
    for (auto slot = 0u; slot < MaxNumColorAttachments; slot++)
    {
        color_formats[slot] = get_format(signature.color_formats[slot]);
        if (color_formats[slot] != VK_FORMAT_UNDEFINED)
        {
            color_attachment_count = slot + 1;
        }
    }

    VkPipelineRenderingCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    create_info.colorAttachmentCount = color_attachment_count;
    create_info.pColorAttachmentFormats = color_formats.data();
    create_info.depthAttachmentFormat = get_format(signature.depth_format);
    return create_info;
}

// lowers the pass's RHIRenderingInfo to dynamic rendering
class VulkanRenderPass : public RHIRenderPass
{
public:
    using RHIRenderPass::RHIRenderPass;

    void Begin();

    void End();
};
} // namespace sparkle

#endif
