#include "rhi/RHIComputePass.h"

#include "rhi/RHI.h"

namespace sparkle
{
RHIComputePass::RHIComputePass(RHIContext *rhi, bool need_timestamp, const std::string &name)
    : RHIResource(name), need_timestamp_(need_timestamp)
{
    execution_time_ms_.resize(rhi->GetMaxFramesInFlight(), -1.0);
}
} // namespace sparkle
