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

    void Begin(RHICommandContext &command_context) override;

    void End(RHICommandContext &command_context) override;

    void TryGetResult() override;

private:
    VkQueryPool query_pool_ = VK_NULL_HANDLE;

    float timestamp_period_ns_;

    uint64_t timestamp_mask_;
};
} // namespace sparkle

#endif
