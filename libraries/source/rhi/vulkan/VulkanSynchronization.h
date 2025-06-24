#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

namespace sparkle
{
class VulkanFence
{
public:
    explicit VulkanFence(VkFence fence) : fence_(fence)
    {
    }

    void Wait() const;

private:
    VkFence fence_ = nullptr;
};
} // namespace sparkle

#endif
