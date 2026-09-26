#if ENABLE_VULKAN

#include "VulkanTimer.h"

#include "VulkanCommon.h"
#include "VulkanContext.h"

#include <limits>

namespace sparkle
{
VulkanTimer::VulkanTimer(const std::string &name) : RHITimer(name)
{
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(context->GetPhysicalDevice(), &properties);
    timestamp_period_ns_ = properties.limits.timestampPeriod;

    // the bits above timestampValidBits are zero, so the counter wraps within the mask
    const auto valid_bits = context->GetTimestampValidBits();
    ASSERT(valid_bits > 0);
    timestamp_mask_ = valid_bits >= 64 ? std::numeric_limits<uint64_t>::max() : (uint64_t{1} << valid_bits) - 1;

    VkQueryPoolCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    create_info.queryCount = 2;

    CHECK_VK_ERROR(vkCreateQueryPool(context->GetDevice(), &create_info, nullptr, &query_pool_));
}

VulkanTimer::~VulkanTimer()
{
    ASSERT(query_pool_ != VK_NULL_HANDLE);

    vkDestroyQueryPool(context->GetDevice(), query_pool_, nullptr);
}

void VulkanTimer::Begin(RHICommandContext &command_context)
{
    ASSERT(status_ != Status::Measuring);

    auto *command_buffer = static_cast<VulkanCommandContext &>(command_context).GetCommandBuffer();

    vkCmdResetQueryPool(command_buffer, query_pool_, 0, 2);

    vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool_, 0);

    status_ = Status::Measuring;
}

void VulkanTimer::End(RHICommandContext &command_context)
{
    ASSERT_EQUAL(status_, Status::Measuring);

    vkCmdWriteTimestamp(static_cast<VulkanCommandContext &>(command_context).GetCommandBuffer(),
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool_, 1);

    status_ = Status::WaitingForResult;
}

void VulkanTimer::TryGetResult()
{
    if (status_ == Status::Ready)
    {
        return;
    }

    ASSERT_EQUAL(status_, Status::WaitingForResult);

    uint64_t timestamps[2];
    auto result = vkGetQueryPoolResults(context->GetDevice(), query_pool_, 0, 2, sizeof(timestamps), timestamps,
                                        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

    if (result == VK_NOT_READY)
    {
        return;
    }

    const uint64_t time_diff_ticks = (timestamps[1] - timestamps[0]) & timestamp_mask_;
    cached_time_ms_ = static_cast<float>(time_diff_ticks) * timestamp_period_ns_ / 1e6f;

    status_ = Status::Ready;
}
} // namespace sparkle

#endif
