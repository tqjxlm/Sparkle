#if ENABLE_VULKAN

#include "VulkanTimer.h"

#include "VulkanCommon.h"
#include "VulkanContext.h"

namespace sparkle
{
VulkanTimer::VulkanTimer(const std::string &name) : RHITimer(name)
{
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(context->GetPhysicalDevice(), &properties);
    timestamp_period_ns_ = properties.limits.timestampPeriod;

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

void VulkanTimer::Begin()
{
    ASSERT(status_ != Status::Measuring);

    vkCmdResetQueryPool(context->GetCurrentCommandBuffer(), query_pool_, 0, 2);

    vkCmdWriteTimestamp(context->GetCurrentCommandBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool_, 0);

    status_ = Status::Measuring;
}

void VulkanTimer::End()
{
    ASSERT_EQUAL(status_, Status::Measuring);

    vkCmdWriteTimestamp(context->GetCurrentCommandBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool_, 1);

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

    uint64_t time_diff_ns = timestamps[1] - timestamps[0];
    cached_time_ms_ = static_cast<float>(time_diff_ns) * timestamp_period_ns_ / 1e6f;

    status_ = Status::Ready;
}
} // namespace sparkle

#endif
