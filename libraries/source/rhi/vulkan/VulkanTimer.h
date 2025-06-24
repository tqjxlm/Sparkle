#pragma once

#if ENABLE_VULKAN

#include "rhi/RHITimer.h"

#include "rhi/VulkanRHI.h"

namespace sparkle
{
class VulkanTimer : public RHITimer
{
public:
    explicit VulkanTimer(const std::string &name);

    ~VulkanTimer() override;

    void Begin() override;

    void End() override;

    void TryGetResult() override;

private:
    VkQueryPool query_pool_ = VK_NULL_HANDLE;

    float timestamp_period_ns_;
};
} // namespace sparkle

#endif
