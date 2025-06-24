#if ENABLE_VULKAN

#include "VulkanSynchronization.h"

#include "VulkanContext.h"

namespace sparkle
{
void VulkanFence::Wait() const
{
    while (vkGetFenceStatus(context->GetDevice(), fence_) != VK_SUCCESS)
    {
        vkWaitForFences(context->GetDevice(), 1, &fence_, VK_TRUE, std::numeric_limits<uint64_t>::max());
    }

    ASSERT_EQUAL(vkGetFenceStatus(context->GetDevice(), fence_), VK_SUCCESS);
}
} // namespace sparkle

#endif
