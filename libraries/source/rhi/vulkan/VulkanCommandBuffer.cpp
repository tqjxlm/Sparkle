#if ENABLE_VULKAN

#include "VulkanCommandBuffer.h"

#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanSynchronization.h"

namespace sparkle
{
OneShotCommandBufferScope::OneShotCommandBufferScope(bool use_external_if_possible, bool should_block_next_frame)
    : should_block_next_frame_(should_block_next_frame)
{
    // if there is an active command buffer, just use it
    if (use_external_if_possible && context->GetCurrentCommandBuffer())
    {
        resources_.command_buffer = context->GetCurrentCommandBuffer();
        use_external_command_buffer_ = true;
        return;
    }

    VkCommandBufferAllocateInfo alloc_info{};

    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = context->GetCommandPool();
    alloc_info.commandBufferCount = 1;

    CHECK_VK_ERROR(vkAllocateCommandBuffers(context->GetDevice(), &alloc_info, &resources_.command_buffer));

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    CHECK_VK_ERROR(vkBeginCommandBuffer(resources_.command_buffer, &begin_info));
}

OneShotCommandBufferScope::~OneShotCommandBufferScope()
{
    // the command buffer is not managed by this class
    if (use_external_command_buffer_)
    {
        ASSERT(context->GetCurrentCommandBuffer() == resources_.command_buffer);
        return;
    }

    CHECK_VK_ERROR(vkEndCommandBuffer(resources_.command_buffer));

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    CHECK_VK_ERROR(vkCreateFence(context->GetDevice(), &fence_info, nullptr, &resources_.fence));

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &resources_.command_buffer;

    CHECK_VK_ERROR(vkQueueSubmit(context->GetGraphicsQueue(), 1, &submit_info, resources_.fence));

    if (should_block_next_frame_)
    {
        VulkanFence fence(resources_.fence);
        context->GetRHI()->EnqueueBeforeFrameTasks([fence]() { fence.Wait(); });
    }

    // deferred deletion
    context->EnqueueCommandBufferResource(std::move(resources_));
}

OneShotCommandBufferScope::CommandBufferResources::~CommandBufferResources()
{
    if (command_buffer)
    {
        vkFreeCommandBuffers(context->GetDevice(), context->GetCommandPool(), 1, &command_buffer);
    }
    if (fence)
    {
        // vkResetFences(rhi->GetDevice(), 1, &resources.fence);
        vkDestroyFence(context->GetDevice(), fence, nullptr);
    }
}

bool OneShotCommandBufferScope::CommandBufferResources::Finished() const
{
    return fence ? vkGetFenceStatus(context->GetDevice(), fence) != VK_NOT_READY : true;
}
} // namespace sparkle

#endif
