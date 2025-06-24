#pragma once

#if ENABLE_VULKAN

#include "VulkanImage.h"

#include <unordered_set>

namespace sparkle
{
class VulkanRenderTarget;

struct QueueFamilyIndices
{
    uint32_t graphicsFamily = 0xffffffff;
    uint32_t presentFamily = 0xffffffff;

    [[nodiscard]] bool IsComplete() const
    {
        return graphicsFamily != 0xffffffff && presentFamily != 0xffffffff;
    }
};

inline QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    QueueFamilyIndices indices;

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);

    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

    // find queue families that we need
    for (uint32_t i = 0; i < queue_family_count; i++)
    {
        const auto &queue_family = queue_families[i];
        if (queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            indices.graphicsFamily = i;
        }

        VkBool32 present_support = 0u;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);
        if (present_support)
        {
            indices.presentFamily = i;
        }

        if (indices.IsComplete())
        {
            break;
        }
    }

    ASSERT(indices.IsComplete());

    return indices;
}

struct SwapChainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

inline SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    SwapChainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);

    if (format_count != 0)
    {
        details.formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, details.formats.data());
    }

    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, nullptr);

    if (present_mode_count != 0)
    {
        details.presentModes.resize(present_mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, details.presentModes.data());
    }

    return details;
}

class VulkanSwapChain
{
public:
    VulkanSwapChain()
    {
        Recreate();
    }

    ~VulkanSwapChain();

    void Recreate();

    void SwapBuffers(VkSemaphore semaphore);

    void RegisterRenderTarget(VulkanRenderTarget *rt)
    {
        regitsered_rt_.insert(rt);
    }

    void UnRegisterRenderTarget(VulkanRenderTarget *rt)
    {
        regitsered_rt_.erase(rt);
    }

    [[nodiscard]] uint32_t GetCurrentImageIndex() const
    {
        return swap_chain_index_;
    }

    [[nodiscard]] VkSwapchainKHR GetSwapchain() const
    {
        return swap_chain_;
    }

    [[nodiscard]] RHIResourceRef<VulkanImage> GetImage(unsigned frame_index) const
    {
        ASSERT_F(frame_index < swap_chain_images_.size(),
                 "Trying to acquire swapchain image[{}] over swapchain capacity {}", frame_index,
                 swap_chain_images_.size());
        return swap_chain_images_[frame_index];
    }

    [[nodiscard]] uint8_t GetSwapChainImageCount() const
    {
        return static_cast<uint8_t>(swap_chain_images_.size());
    }

    [[nodiscard]] VkExtent2D GetExtent() const
    {
        return image_extent_;
    }

    [[nodiscard]] VkFormat GetFormat() const
    {
        return color_format_;
    }

private:
    VkSwapchainKHR swap_chain_;
    uint32_t swap_chain_index_ = 0;
    std::vector<RHIResourceRef<VulkanImage>> swap_chain_images_;

    std::unordered_set<VulkanRenderTarget *> regitsered_rt_;

    VkExtent2D image_extent_;
    VkFormat color_format_;
};
} // namespace sparkle

#endif
