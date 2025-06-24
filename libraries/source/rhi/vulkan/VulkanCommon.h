#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#ifndef NDEBUG
#define VMA_LEAK_LOG_FORMAT(format, ...)                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        printf((format), __VA_ARGS__);                                                                                 \
        printf("\n");                                                                                                  \
    } while (false)
#endif

#define CHECK_VK_ERROR(vk_result, ...) ASSERT_EQUAL(vk_result, VK_SUCCESS)

namespace sparkle
{
inline void CheckVkResult(VkResult err)
{
    if (err == 0)
        return;

    ASSERT_F(false, "[vulkan] Error: VkResult = {}", static_cast<int>(err));
}

inline const uint32_t ApiVersion = VK_API_VERSION_1_1;

template <class T> void ChainVkStructurePtr(VkDeviceCreateInfo &info, T &next)
{
    next.pNext = const_cast<void *>(info.pNext);
    info.pNext = &next;
}
} // namespace sparkle

#endif
