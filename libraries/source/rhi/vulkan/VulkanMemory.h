#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#if VULKAN_USE_VOLK
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#endif

#include <vk_mem_alloc.h>

namespace sparkle
{

inline VkMemoryPropertyFlags GetVulkanMemoryPropertyFlags(RHIMemoryProperty properties)
{
    VkMemoryPropertyFlags flags = 0;
    if (properties & RHIMemoryProperty::DeviceLocal)
    {
        flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
    if (properties & RHIMemoryProperty::HostVisible)
    {
        flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    }
    if (properties & RHIMemoryProperty::HostCoherent)
    {
        flags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    if (properties & RHIMemoryProperty::HostCached)
    {
        flags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }

    return flags;
}

} // namespace sparkle

#endif
