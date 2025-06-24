#if FRAMEWORK_APPLE

#include "MetalTimer.h"

#include "MetalComputePass.h"
#include "MetalContext.h"
#include "MetalRenderPass.h"
#include "core/Exception.h"

namespace sparkle
{
MetalTimer::MetalTimer(const std::string &name) : RHITimer(name)
{
    ASSERT_F(false, "We do not have a good implementation for metal before Apple improves the API. The current "
                    "implementation is how we expect it to look like if it works.");

    @autoreleasepool
    {
        auto device = context->GetDevice();

        // Find the timestamp counter set
        for (id<MTLCounterSet> counter_set in device.counterSets)
        {
            if ([counter_set.name isEqualToString:@"timestamp"])
            {
                timestamp_counter_set_ = counter_set;
                break;
            }
        }

        ASSERT(timestamp_counter_set_ != nil);

        // Create counter sample buffer with 2 samples (begin and end)
        MTLCounterSampleBufferDescriptor *descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
        descriptor.counterSet = timestamp_counter_set_;
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.sampleCount = 2;
        descriptor.label = [NSString stringWithUTF8String:name.c_str()];

        NSError *error = nil;
        counter_sample_buffer_ = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
        ASSERT(counter_sample_buffer_ != nil);
    }
}

MetalTimer::~MetalTimer()
{
    @autoreleasepool
    {
        counter_sample_buffer_ = nil;
        timestamp_counter_set_ = nil;
    }
}

static id<MTLCommandEncoder> GetEncoderForCurrentPass()
{
    // Try to get current encoder from RHI context
    auto compute_pass = context->GetRHI()->GetCurrentComputePass();
    auto render_pass = context->GetRHI()->GetCurrentRenderPass();

    id<MTLCommandEncoder> encoder = nil;

    if (compute_pass)
    {
        auto *metal_pass = reinterpret_cast<MetalComputePass *>(compute_pass.get());
        encoder = metal_pass->GetEncoder();
    }
    else if (render_pass)
    {
        auto *metal_pass = reinterpret_cast<MetalRenderPass *>(render_pass.get());
        encoder = metal_pass->GetRenderEncoder();
    }

    ASSERT_F(encoder != nil, "No active encoder for GPU timer - timer must be used within a render or compute pass");

    ASSERT_F([encoder respondsToSelector:@selector(sampleCountersInBuffer:atSampleIndex:withBarrier:)],
             "sampleCountersInBuffer method not available on encoder");

    return encoder;
}

void MetalTimer::Begin()
{
    ASSERT(status_ != Status::Measuring);

    @autoreleasepool
    {
        id<MTLCommandEncoder> encoder = GetEncoderForCurrentPass();

        [(id)encoder sampleCountersInBuffer:counter_sample_buffer_ atSampleIndex:0 withBarrier:NO];

        status_ = Status::Measuring;
    }
}

void MetalTimer::End()
{
    ASSERT_EQUAL(status_, Status::Measuring);

    @autoreleasepool
    {
        id<MTLCommandEncoder> encoder = GetEncoderForCurrentPass();

        [(id)encoder sampleCountersInBuffer:counter_sample_buffer_ atSampleIndex:1 withBarrier:NO];

        status_ = Status::WaitingForResult;
    }
}

void MetalTimer::TryGetResult()
{
    if (status_ == Status::Ready)
    {
        return;
    }

    ASSERT_EQUAL(status_, Status::WaitingForResult);

    @autoreleasepool
    {
        // Resolve the counter sample buffer to get the timestamp data
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
        cached_time_ms_ = static_cast<float>(elapsed_ns) / 1000000.0f;

        status_ = Status::Ready;
    }
}
} // namespace sparkle

#endif