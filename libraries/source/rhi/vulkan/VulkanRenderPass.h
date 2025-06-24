#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

namespace sparkle
{
inline VkAttachmentLoadOp GetAttachmentLoadOp(RHIRenderPass::LoadOp op)
{
    switch (op)
    {
    case RHIRenderPass::LoadOp::None:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    case RHIRenderPass::LoadOp::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case RHIRenderPass::LoadOp::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    }
}

inline VkAttachmentStoreOp GetAttachmentStoreOp(RHIRenderPass::StoreOp op)
{
    switch (op)
    {
    case RHIRenderPass::StoreOp::None:
        return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    case RHIRenderPass::StoreOp::Store:
        return VK_ATTACHMENT_STORE_OP_STORE;
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

class VulkanRenderPass : public RHIRenderPass
{
public:
    VulkanRenderPass(const RHIRenderPass::Attribute &attribute, const RHIResourceRef<RHIRenderTarget> &rt,
                     const std::string &name);

    ~VulkanRenderPass() override
    {
        Cleanup();
    }

    [[nodiscard]] VkRenderPass GetRenderPass() const
    {
        return render_pass_;
    }

    void Init(const RHIResourceRef<RHIRenderTarget> &rt);

    void Cleanup();

    void Begin();

    [[nodiscard]] bool RequireBackBuffer() const
    {
        return require_back_buffer_;
    }

    void End();

private:
    void CreateRenderPass();

    void CreateFramebuffers();

    VkRenderPass render_pass_;
    std::vector<VkFramebuffer> frame_buffers_;
    bool require_back_buffer_;
};
} // namespace sparkle

#endif
