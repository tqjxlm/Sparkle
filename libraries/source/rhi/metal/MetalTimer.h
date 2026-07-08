#pragma once

#if FRAMEWORK_APPLE

#include "rhi/RHITimer.h"

#include "MetalRHIInternal.h"

#include <atomic>

namespace sparkle
{
// MTLCounterSampleBuffer timing. Apple GPUs only sample timestamps at encoder boundaries, so a
// Metal timer measures one whole pass: AttachTo the pass descriptor before creating its encoder,
// then bracket with Begin/End; the samples themselves are taken by the GPU at encoder start/end.
class MetalTimer : public RHITimer
{
public:
    explicit MetalTimer(const std::string &name);

    void Begin() override;

    void End() override;

    void TryGetResult() override;

    void AttachTo(MTLComputePassDescriptor *descriptor) const;

private:
    id<MTLCounterSampleBuffer> counter_sample_buffer_ = nil;
    std::atomic<bool> resolved_ = false;
    std::atomic<float> resolved_time_ms_ = 0.f;
};
} // namespace sparkle

#endif
