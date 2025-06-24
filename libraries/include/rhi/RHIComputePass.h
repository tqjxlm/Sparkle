#pragma once

#include "rhi/RHIResource.h"

#include <vector>

namespace sparkle
{
class RHIComputePass : public RHIResource
{
public:
    RHIComputePass(RHIContext *rhi, bool need_timestamp, const std::string &name);

    ~RHIComputePass() override = default;

    [[nodiscard]] auto GetExecutionTime(unsigned frame_index) const
    {
        return execution_time_ms_[frame_index];
    }

protected:
    bool need_timestamp_ = false;

    std::vector<float> execution_time_ms_;
};
} // namespace sparkle
