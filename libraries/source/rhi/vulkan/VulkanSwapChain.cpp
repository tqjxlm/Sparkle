#if ENABLE_VULKAN

#include "VulkanSwapChain.h"

#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanRenderTarget.h"
#include "application/NativeView.h"
#include "core/Exception.h"
#include "core/math/Utilities.h"

namespace sparkle
{
static VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats)
{
    for (const auto &available_format : availableFormats)
    {
        if ((available_format.format == VK_FORMAT_B8G8R8A8_SRGB ||
             available_format.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            available_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return available_format;
        }
    }

    return availableFormats[0];
}

static VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities, NativeView *window)
{
    if (capabilities.currentExtent.width != UINT32_MAX)
    {
        return capabilities.currentExtent;
    }

    int width;
    int height;
    window->GetFrameBufferSize(width, height);

    VkExtent2D actual_extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    actual_extent.width =
        std::clamp(actual_extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    actual_extent.height =
        std::clamp(actual_extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

    return actual_extent;
}

static VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR> &availablePresentModes,
                                              bool use_vsync)
{
    if (use_vsync)
    {
        Log(Info, "VSync On");
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    for (const auto &available_present_mode : availablePresentModes)
    {
        if (available_present_mode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            return available_present_mode;
        }
    }
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
}

VulkanSwapChain::~VulkanSwapChain()
{
    if (!swap_chain_images_.empty())
    {
        swap_chain_images_.clear();

        vkDestroySwapchainKHR(context->GetDevice(), swap_chain_, nullptr);
    }
}

void VulkanSwapChain::Recreate()
{
    SwapChainSupportDetails const swap_chain_support =
        QuerySwapChainSupport(context->GetPhysicalDevice(), context->GetSurface());

    VkSurfaceFormatKHR const surface_format = ChooseSwapSurfaceFormat(swap_chain_support.formats);
    VkPresentModeKHR const present_mode =
        ChooseSwapPresentMode(swap_chain_support.presentModes, context->GetRHI()->GetConfig().use_vsync);
    VkExtent2D extent = ChooseSwapExtent(swap_chain_support.capabilities, context->GetRHI()->GetHardwareInterface());

    if (context->GetRHI()->GetConfig().enable_pre_transform)
    {
        if (swap_chain_support.capabilities.currentTransform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
            swap_chain_support.capabilities.currentTransform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
        {
            // Swap to get identity width and height
            Log(Debug, "Before pre-rotation window size {} {}. Orientation {}", extent.width, extent.height,
                static_cast<int>(swap_chain_support.capabilities.currentTransform));
            utilities::Swap(extent.width, extent.height);
        }

        if (swap_chain_support.capabilities.currentTransform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)
        {
            context->GetRHI()->GetHardwareInterface()->SetWindowRotation(NativeView::WindowRotation::Landscape);
        }
        else if (swap_chain_support.capabilities.currentTransform & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)
        {
            context->GetRHI()->GetHardwareInterface()->SetWindowRotation(NativeView::WindowRotation::ReversePortrait);
        }
        else if (swap_chain_support.capabilities.currentTransform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
        {
            context->GetRHI()->GetHardwareInterface()->SetWindowRotation(NativeView::WindowRotation::ReverseLandscape);
        }
        else
        {
            context->GetRHI()->GetHardwareInterface()->SetWindowRotation(NativeView::WindowRotation::Portrait);
        }
    }

    Log(Info, "Swap chain extent {} {}. Format {}", extent.width, extent.height,
        static_cast<int>(surface_format.format));

    color_format_ = surface_format.format;
    image_extent_ = extent;

    uint32_t image_count = swap_chain_support.capabilities.minImageCount + 1;

    if (swap_chain_support.capabilities.maxImageCount > 0 &&
        image_count > swap_chain_support.capabilities.maxImageCount)
    {
        image_count = swap_chain_support.capabilities.maxImageCount;
    }

    ASSERT(swap_chain_support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ASSERT(swap_chain_support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = context->GetSurface();

    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (context->GetRHI()->GetConfig().enable_pre_transform)
    {
        create_info.preTransform = swap_chain_support.capabilities.currentTransform;
    }
    else
    {
        create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }

    QueueFamilyIndices const indices = FindQueueFamilies(context->GetPhysicalDevice(), context->GetSurface());
    uint32_t queue_family_indices[] = {indices.graphicsFamily, indices.presentFamily};

    if (indices.graphicsFamily != indices.presentFamily)
    {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_family_indices;
    }
    else
    {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.queueFamilyIndexCount = 0;     // Optional
        create_info.pQueueFamilyIndices = nullptr; // Optional
    }

    if (swap_chain_support.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
    {
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }
    else
    {
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }

    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;

    create_info.oldSwapchain = VK_NULL_HANDLE;

    CHECK_VK_ERROR(vkCreateSwapchainKHR(context->GetDevice(), &create_info, nullptr, &swap_chain_));

    vkGetSwapchainImagesKHR(context->GetDevice(), swap_chain_, &image_count, nullptr);
    std::vector<VkImage> created_images(image_count);
    vkGetSwapchainImagesKHR(context->GetDevice(), swap_chain_, &image_count, created_images.data());

    Log(Info, "Number of swap chain images: {}", image_count);
    context->GetRHI()->SetMaxFramesInFlight(image_count);

    swap_chain_images_.resize(image_count);
    for (auto i = 0u; i < image_count; i++)
    {
        RHIImage::Attribute attribute;
        attribute.width = extent.width;
        attribute.height = extent.height;
        attribute.mip_levels = 1;
        attribute.msaa_samples = 1;
        attribute.usages = RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::TransferDst;

        swap_chain_images_[i] = context->GetRHI()->CreateResource<VulkanImage>(attribute, color_format_,
                                                                               created_images[i], "SwapchainImage");
    }
}

void VulkanSwapChain::SwapBuffers(VkSemaphore semaphore)
{
    // Get the next image index from the swap chain
    // Note: this is an async request, the image is not available until the semaphore is ready
    const VkResult result = vkAcquireNextImageKHR(context->GetDevice(), swap_chain_, UINT64_MAX, semaphore,
                                                  VK_NULL_HANDLE, &swap_chain_index_);

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        Log(Info, "recreate swapchain because of VK_ERROR_OUT_OF_DATE_KHR");

        Recreate();
        for (auto *rt : regitsered_rt_)
        {
            rt->Recreate();
        }
    }
    else
    {
        for (auto *rt : regitsered_rt_)
        {
            rt->SyncWithSwapChain();
        }
    }

    ASSERT_F(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "failed to acquire swap chain image!");
}
} // namespace sparkle

#endif
