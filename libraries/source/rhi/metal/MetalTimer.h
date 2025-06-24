#pragma once

#if FRAMEWORK_APPLE

#include "rhi/RHITimer.h"

#include "MetalRHIInternal.h"

namespace sparkle
{
// Use MTLCounterSampleBuffer for GPU timing (macOS 10.15+, iOS 13.0+)
class MetalTimer : public RHITimer
{
public:
    explicit MetalTimer(const std::string &name);

    ~MetalTimer() override;

    void Begin() override;

    void End() override;

    void TryGetResult() override;

private:
    id<MTLCounterSampleBuffer> counter_sample_buffer_ = nil;
    id<MTLCounterSet> timestamp_counter_set_ = nil;
};
} // namespace sparkle

#endif
