#pragma once

#if FRAMEWORK_APPLE

#include "rhi/RHITimer.h"

#include "MetalRHIInternal.h"

#include <array>
#include <atomic>

namespace sparkle
{
// MTLCounterSampleBuffer timing. Apple GPUs only sample timestamps at stage boundaries, so a Metal timer measures one
// whole pass: AttachTo the pass descriptor before creating its encoder, then bracket with Begin/End; the samples
// themselves are taken by the GPU where the encoder's stages start and end.
class MetalTimer : public RHITimer
{
public:
    explicit MetalTimer(const std::string &name);

    // the device has a timestamp counter set that it samples at stage boundaries
    static bool IsSupported(id<MTLDevice> device);

    void Begin(RHICommandContext &command_context) override;

    void End(RHICommandContext &command_context) override;

    void TryGetResult() override;

    void AttachTo(MTLComputePassDescriptor *descriptor);

    void AttachTo(MTLRenderPassDescriptor *descriptor);

private:
    // a render pass samples the start and end of its vertex and fragment stages
    static constexpr NSUInteger MaxSampleCount = 4;

    id<MTLCounterSampleBuffer> counter_sample_buffer_ = nil;
    // samples of the attached pass: a start and an end per stage
    NSUInteger sample_count_ = 0;
    // the samples of the last resolve, which a stage that does not run leaves in the buffer
    std::array<uint64_t, MaxSampleCount> previous_samples_{};
    std::atomic<bool> resolved_ = false;
    std::atomic<float> resolved_time_ms_ = 0.f;
};
} // namespace sparkle

#endif
