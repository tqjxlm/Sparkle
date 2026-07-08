#if FRAMEWORK_APPLE

#include "MetalTimer.h"

#include "MetalContext.h"
#include "core/Exception.h"

namespace sparkle
{
MetalTimer::MetalTimer(const std::string &name) : RHITimer(name)
{
    @autoreleasepool
    {
        auto device = context->GetDevice();

        id<MTLCounterSet> timestamp_counter_set = nil;
        for (id<MTLCounterSet> counter_set in device.counterSets)
        {
            if ([counter_set.name isEqualToString:@"timestamp"])
            {
                timestamp_counter_set = counter_set;
                break;
            }
        }
        ASSERT(timestamp_counter_set != nil);

        MTLCounterSampleBufferDescriptor *descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
        descriptor.counterSet = timestamp_counter_set;
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.sampleCount = 2;
        descriptor.label = [NSString stringWithUTF8String:name.c_str()];

        NSError *error = nil;
        counter_sample_buffer_ = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
        ASSERT_F(counter_sample_buffer_ != nil, "Failed to create counter sample buffer. Error: {}",
                 error ? [error.localizedDescription UTF8String] : "unknown");
    }
}

void MetalTimer::AttachTo(MTLComputePassDescriptor *descriptor) const
{
    MTLComputePassSampleBufferAttachmentDescriptor *attachment = descriptor.sampleBufferAttachments[0];
    attachment.sampleBuffer = counter_sample_buffer_;
    attachment.startOfEncoderSampleIndex = 0;
    attachment.endOfEncoderSampleIndex = 1;
}

void MetalTimer::Begin()
{
    ASSERT(status_ != Status::Measuring);

    status_ = Status::Measuring;
}

void MetalTimer::End()
{
    ASSERT_EQUAL(status_, Status::Measuring);

    id<MTLCounterSampleBuffer> buffer = counter_sample_buffer_;
    std::atomic<float> *time_slot = &resolved_time_ms_;
    std::atomic<bool> *resolved = &resolved_;
    [context->GetCurrentCommandBuffer() addCompletedHandler:^(id<MTLCommandBuffer>) {
      NSData *data = [buffer resolveCounterRange:NSMakeRange(0, 2)];
      if (!data)
      {
          return;
      }

      const auto *timestamps = static_cast<const uint64_t *>(data.bytes);
      *time_slot = static_cast<float>(timestamps[1] - timestamps[0]) / 1000000.0f;
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
