#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

namespace sparkle
{
class VulkanSwapChain;

class VulkanRenderTarget : public RHIRenderTarget
{
public:
    VulkanRenderTarget(const RHIRenderTarget::Attribute &attribute, const RHIResourceRef<RHIImage> &depth_image,
                       const std::string &name);

    VulkanRenderTarget(const Attribute &attribute, const RHIRenderTarget::ColorImageArray &color_images,
                       const RHIResourceRef<RHIImage> &depth_image, const std::string &name);

    ~VulkanRenderTarget() override;

    [[nodiscard]] VkExtent2D GetExtent() const
    {
        return image_extent_;
    }

    [[nodiscard]] VkFormat GetFormat(size_t index) const
    {
        return color_formats_[index];
    }

    [[nodiscard]] std::vector<VkImageView> GetAttachments(unsigned frame_index) const;

    void SyncWithSwapChain();

    void Recreate();

private:
    void CreateMsaaResources();

    VkExtent2D image_extent_;
    std::array<VkFormat, RHIRenderTarget::MaxNumColorImage> color_formats_;

    VulkanSwapChain *swap_chain_ = nullptr;
};
} // namespace sparkle

#endif
