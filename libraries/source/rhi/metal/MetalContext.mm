#if FRAMEWORK_APPLE

#include "MetalContext.h"

#include "MetalImage.h"

namespace sparkle
{
void MetalContext::SwapBuffer()
{
    back_buffer_color_->SetImage(current_drawable_.texture);
}

void MetalContext::CreateBackBuffer()
{
    RHIImage::Attribute attribute;
    attribute.width = view_.drawableSize.width;
    attribute.height = view_.drawableSize.height;
    attribute.mip_levels = 1;
    attribute.msaa_samples = 1;
    attribute.format = PixelFormat::B8G8R8A8_SRGB;
    attribute.usages = RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::TransientAttachment;
    attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                         .filtering_method_min = RHISampler::FilteringMethod::Linear,
                         .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                         .filtering_method_mipmap = RHISampler::FilteringMethod::Linear};
    attribute.memory_properties = RHIMemoryProperty::DeviceLocal;

    back_buffer_color_ = context->GetRHI()->CreateResource<MetalImage>(attribute, nullptr, "BackBufferColor");
}

MetalContext::MetalContext(MetalRHI *context, MetalView *mtk_view) : view_(mtk_view), rhi_(context)
{
    device_ = view_.device;
    command_queue_ = [device_ newCommandQueue];
    current_drawable_ = [view_ currentDrawable];

    rhi_->SetMaxFramesInFlight([view_ getMaxFrameInFlight]);
}

void MetalContext::BeginFrame()
{
    current_drawable_ = [view_ getNextDrawable];

    SwapBuffer();

    if (num_frames_to_capture_ > 0)
    {
        BeginFrameCapture();
    }

    BeginCommandBuffer();
}

void MetalContext::EndFrame()
{
    [current_command_buffer_ presentDrawable:current_drawable_];

    [current_command_buffer_ addCompletedHandler:^(id<MTLCommandBuffer>) {
      dispatch_semaphore_signal([view_ getInFlightSemaphore]);
    }];

    SubmitCommandBuffer();

    if (is_capturing_frame_)
    {
        EndFrameCapture();
    }
}

void MetalContext::BeginFrameCapture()
{
    ASSERT(num_frames_to_capture_ > 0);

    NSError *error = nullptr;

    NSURL *app_support_dir = [[NSFileManager defaultManager] URLForDirectory:NSDocumentDirectory
                                                                    inDomain:NSUserDomainMask
                                                           appropriateForURL:nil
                                                                      create:YES
                                                                       error:&error];
    if (error)
    {
        Log(Error, "failed to prepare directory for saving frame capture. error: {}",
            [error.localizedDescription UTF8String]);
        num_frames_to_capture_ = 0;
        return;
    }

    NSDate *current_time = [NSDate date];
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    [formatter setDateFormat:@"yyyy-MM-dd-HH-mm"];
    NSString *time_string = [formatter stringFromDate:current_time];

    NSURL *capture_output_url =
        [app_support_dir URLByAppendingPathComponent:[NSString stringWithFormat:@"capture/%@.gputrace", time_string]
                                         isDirectory:YES];

    auto capture_manager = [MTLCaptureManager sharedCaptureManager];

    auto capture_descriptor = [[MTLCaptureDescriptor alloc] init];
    capture_descriptor.captureObject = device_;
    capture_descriptor.destination = MTLCaptureDestination::MTLCaptureDestinationGPUTraceDocument;
    capture_descriptor.outputURL = capture_output_url;

    auto success = [capture_manager startCaptureWithDescriptor:capture_descriptor error:&error];
    if (success)
    {
        Log(Info, "begine frame capture");
        is_capturing_frame_ = true;
    }
    else
    {
        Log(Error, "failed to capture frame. error: {}", [error.localizedDescription UTF8String]);
        num_frames_to_capture_ = 0;
    }
}

void MetalContext::EndFrameCapture()
{
    auto capture_manager = [MTLCaptureManager sharedCaptureManager];

    ASSERT(is_capturing_frame_);
    ASSERT(capture_manager.isCapturing);

    [capture_manager stopCapture];

    num_frames_to_capture_--;
    is_capturing_frame_ = false;

    Log(Info, "end frame capture. remaining {}", num_frames_to_capture_);
}

void MetalContext::SubmitCommandBuffer()
{
    [current_command_buffer_ commit];
    last_command_buffer_ = current_command_buffer_;
    current_command_buffer_ = nullptr;
}

void MetalContext::BeginCommandBuffer()
{
    ASSERT_F(!IsInCommandBuffer(), "already in a command buffer");
    current_command_buffer_ = [command_queue_ commandBuffer];
}

void MetalContext::CaptureNextFrames(int count)
{
    if (num_frames_to_capture_ > 0)
    {
        Log(Warn, "cannot capture frame when the previous capture has not finished. Remaining frames to capture {}",
            num_frames_to_capture_);
        return;
    }

    num_frames_to_capture_ = count;
}

void MetalContext::WaitUntilDeviceIdle()
{
    if (last_command_buffer_)
    {
        // the execution in command queue is sequential.
        // by waiting on the last command buffer, we effectively wait for the whole queue.
        // we don't wait for the current queue because it has not been submitted yet.
        [last_command_buffer_ waitUntilCompleted];
    }
}
} // namespace sparkle

#endif
