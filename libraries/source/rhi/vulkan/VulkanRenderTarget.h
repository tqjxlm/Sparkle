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
