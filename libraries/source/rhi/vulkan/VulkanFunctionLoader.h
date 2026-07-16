#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

namespace sparkle
{
struct VulkanFunctionLoader
{
    static bool Init();
    static void LoadInstance(VkInstance instance);
    static void LoadDevice(VkDevice device, bool enable_ray_tracing, bool enable_debug_utils);
};
} // namespace sparkle

#endif
