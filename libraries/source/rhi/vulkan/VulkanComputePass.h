#pragma once

#if ENABLE_VULKAN

#include "rhi/RHIComputePass.h"

#include "rhi/RHITimer.h"

namespace sparkle
{
class VulkanComputePass : public RHIComputePass
{
public:
    VulkanComputePass(RHIContext *rhi, bool need_timestamp, const std::string &name);

    ~VulkanComputePass() override = default;

    void Begin();

    void End();

private:
    std::vector<RHIResourceRef<RHITimer>> timers_;
};
} // namespace sparkle

#endif
