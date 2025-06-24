#if FRAMEWORK_APPLE

#include "MetalComputePass.h"

#include "MetalContext.h"

namespace sparkle
{
MetalComputePass::MetalComputePass(RHIContext *rhi, bool need_timestamp, const std::string &name)
    : RHIComputePass(rhi, need_timestamp, name)
{
    descriptor_ = [[MTLComputePassDescriptor alloc] init];
    descriptor_.dispatchType = MTLDispatchTypeSerial;

    if (need_timestamp)
    {
        auto device = context->GetDevice();

        id<MTLCounterSet> timestamp_counter_set;

        // Find the timestamp counter set
        for (id<MTLCounterSet> counter_set in device.counterSets)
        {
            if ([counter_set.name isEqualToString:@"timestamp"])
            {
                timestamp_counter_set = counter_set;
                break;
            }
        }

        ASSERT(timestamp_counter_set != nil);

        // Create counter sample buffer with 2 samples (begin and end)
        MTLCounterSampleBufferDescriptor *descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
        descriptor.counterSet = timestamp_counter_set;
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.sampleCount = 2;
        descriptor.label = [NSString stringWithUTF8String:name.c_str()];

        NSError *error = nil;
        counter_sample_buffer_ = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
        ASSERT_F(counter_sample_buffer_ != nil, "Failed to create counter sample buffer. Error: {}",
                 [error.localizedDescription UTF8String]);

        MTLComputePassSampleBufferAttachmentDescriptor *sample_attachement = descriptor_.sampleBufferAttachments[0];

        sample_attachement.sampleBuffer = counter_sample_buffer_;
        sample_attachement.startOfEncoderSampleIndex = 0;
        sample_attachement.endOfEncoderSampleIndex = 1;
    }
}

void MetalComputePass::Begin()
{
    compute_encoder_ = [context->GetCurrentCommandBuffer() computeCommandEncoderWithDescriptor:descriptor_];
    ASSERT(compute_encoder_);

    SetDebugInfo(compute_encoder_, GetName());
}

void MetalComputePass::End()
{
    [compute_encoder_ endEncoding];

    if (need_timestamp_)
    {
        auto frame_index = context->GetRHI()->GetFrameIndex();
        [context->GetCurrentCommandBuffer() addCompletedHandler:^(id<MTLCommandBuffer>) {
          NSData *data = [counter_sample_buffer_ resolveCounterRange:NSMakeRange(0, 2)];
          if (!data)
          {
              // GPU work not yet complete
              return;
          }

          // Extract timestamp values from the resolved data
          const auto *timestamps = static_cast<const uint64_t *>(data.bytes);
          uint64_t begin_timestamp = timestamps[0];
          uint64_t end_timestamp = timestamps[1];

          // Calculate elapsed time in nanoseconds and convert to milliseconds
          uint64_t elapsed_ns = end_timestamp - begin_timestamp;
          execution_time_ms_[frame_index] = static_cast<float>(elapsed_ns) / 1000000.0f;
        }];
    }
}

} // namespace sparkle

#endif
