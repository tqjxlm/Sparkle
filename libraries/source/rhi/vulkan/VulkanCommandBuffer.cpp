#if ENABLE_VULKAN

#include "VulkanCommandBuffer.h"

#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanSynchronization.h"

namespace sparkle
{
OneShotCommandBufferScope::OneShotCommandBufferScope(bool should_block_next_frame)
    : command_context_(context->GetRHI()), should_block_next_frame_(should_block_next_frame)
{
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

    command_context_.Begin(resources_.command_buffer);
}

OneShotCommandBufferScope::~OneShotCommandBufferScope()
{
    command_context_.End();

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
        vkDestroyFence(context->GetDevice(), fence, nullptr);
    }
}

bool OneShotCommandBufferScope::CommandBufferResources::Finished() const
{
    return fence ? vkGetFenceStatus(context->GetDevice(), fence) != VK_NOT_READY : true;
}
} // namespace sparkle

#endif
