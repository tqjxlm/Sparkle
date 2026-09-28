#if FRAMEWORK_APPLE

#include "MetalTimer.h"

#include "MetalContext.h"
#include "core/Exception.h"

#include <algorithm>
#include <limits>

namespace sparkle
{
static id<MTLCounterSet> FindTimestampCounterSet(id<MTLDevice> device)
{
    for (id<MTLCounterSet> counter_set in device.counterSets)
    {
        if ([counter_set.name isEqualToString:@"timestamp"])
        {
            return counter_set;
        }
    }
    return nil;
}

// starts are at even indices and their ends follow them. a stage that did not run leaves its samples unwritten: zero in
// a new buffer, or the timestamps of the buffer's previous run. Metal skips a compute encoder that dispatched nothing,
// so a pass none of whose stages ran did no GPU work.
static float GetElapsedTimeMs(const MTLCounterResultTimestamp *samples, const uint64_t *previous,
                              NSUInteger sample_count)
{
    auto is_written = [&](NSUInteger i) { return samples[i].timestamp != 0 && samples[i].timestamp != previous[i]; };
    auto is_error = [&](NSUInteger i) { return samples[i].timestamp == MTLCounterErrorValue; };

    uint64_t start = std::numeric_limits<uint64_t>::max();
    uint64_t end = 0;
    bool failed = false;
    for (NSUInteger i = 0; i + 1 < sample_count; i += 2)
    {
        if (is_error(i) || is_error(i + 1))
        {
            failed = true;
        }
        else if (is_written(i) && is_written(i + 1))
        {
            start = std::min(start, samples[i].timestamp);
            end = std::max(end, samples[i + 1].timestamp);
        }
    }

    if (start <= end)
    {
        return static_cast<float>(end - start) / 1000000.0f;
    }
    return failed ? -1.f : 0.f;
}

MetalTimer::MetalTimer(const std::string &name) : RHITimer(name)
{
    @autoreleasepool
    {
        auto device = context->GetDevice();

        id<MTLCounterSet> timestamp_counter_set = FindTimestampCounterSet(device);
        ASSERT(timestamp_counter_set != nil);

        MTLCounterSampleBufferDescriptor *descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
        descriptor.counterSet = timestamp_counter_set;
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.sampleCount = MaxSampleCount;
        descriptor.label = [NSString stringWithUTF8String:name.c_str()];

        NSError *error = nil;
        counter_sample_buffer_ = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
        ASSERT_F(counter_sample_buffer_ != nil, "Failed to create counter sample buffer. Error: {}",
                 error ? [error.localizedDescription UTF8String] : "unknown");
    }
}

bool MetalTimer::IsSupported(id<MTLDevice> device)
{
    return FindTimestampCounterSet(device) != nil &&
           [device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary];
}

void MetalTimer::AttachTo(MTLComputePassDescriptor *descriptor)
{
    MTLComputePassSampleBufferAttachmentDescriptor *attachment = descriptor.sampleBufferAttachments[0];
    attachment.sampleBuffer = counter_sample_buffer_;
    attachment.startOfEncoderSampleIndex = 0;
    attachment.endOfEncoderSampleIndex = 1;
    sample_count_ = 2;
}

void MetalTimer::AttachTo(MTLRenderPassDescriptor *descriptor)
{
    MTLRenderPassSampleBufferAttachmentDescriptor *attachment = descriptor.sampleBufferAttachments[0];
    attachment.sampleBuffer = counter_sample_buffer_;
    attachment.startOfVertexSampleIndex = 0;
    attachment.endOfVertexSampleIndex = 1;
    attachment.startOfFragmentSampleIndex = 2;
    attachment.endOfFragmentSampleIndex = 3;
    sample_count_ = MaxSampleCount;
}

void MetalTimer::Begin(RHICommandContext & /*command_context*/)
{
    ASSERT(status_ != Status::Measuring);

    status_ = Status::Measuring;
}

void MetalTimer::End(RHICommandContext &command_context)
{
    ASSERT_EQUAL(status_, Status::Measuring);

    id<MTLCommandBuffer> command_buffer = static_cast<MetalCommandContext &>(command_context).GetCommandBuffer();
    id<MTLCounterSampleBuffer> buffer = counter_sample_buffer_;
    const NSUInteger sample_count = sample_count_;
    std::atomic<float> *time_slot = &resolved_time_ms_;
    std::atomic<bool> *resolved = &resolved_;
    uint64_t *previous = previous_samples_.data();
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
      NSData *data = [buffer resolveCounterRange:NSMakeRange(0, sample_count)];
      if (data)
      {
          const auto *samples = static_cast<const MTLCounterResultTimestamp *>(data.bytes);
          *time_slot = GetElapsedTimeMs(samples, previous, sample_count);
          for (NSUInteger i = 0; i < sample_count; i++)
          {
              previous[i] = samples[i].timestamp;
          }
      }
      else
      {
          *time_slot = -1.f;
      }
      *resolved = true;
    }];

    status_ = Status::WaitingForResult;
}

void MetalTimer::TryGetResult()
{
    if (status_ == Status::Ready)
    {
        return;
    }

    ASSERT_EQUAL(status_, Status::WaitingForResult);

    if (!resolved_)
    {
        return;
    }

    cached_time_ms_ = resolved_time_ms_;
    resolved_ = false;
    status_ = Status::Ready;
}
} // namespace sparkle

#endif
