#pragma once

#if FRAMEWORK_APPLE

#include "MetalCommandContext.h"
#include "MetalImage.h"
#include "MetalRHIInternal.h"
#import "apple/MetalView.h"

namespace sparkle
{
class MetalContext
{
public:
    MetalContext(MetalRHI *context, MetalView *mtk_view, bool is_headless, uint32_t headless_width,
                 uint32_t headless_height);

    [[nodiscard]] id<MTLDevice> GetDevice() const
    {
        return device_;
    }

    // see RHIContext::GetCommandContext
    [[nodiscard]] MetalCommandContext *GetCommandContext()
    {
        return IsInCommandBuffer() ? &command_context_ : nullptr;
    }

    // a one-off command buffer independent of the frame's, for synchronous transfers
    [[nodiscard]] id<MTLCommandBuffer> CreateStandaloneCommandBuffer() const
    {
        return [command_queue_ commandBuffer];
    }

    [[nodiscard]] RHIResourceRef<MetalImage> GetBackBufferColor() const
    {
        return back_buffer_color_;
    }

    [[nodiscard]] bool IsInCommandBuffer() const
    {
        return command_context_.GetCommandBuffer() != nil;
    }

    [[nodiscard]] MetalView *GetView() const
    {
        return view_;
    }

    [[nodiscard]] CGSize GetDrawableSize() const
    {
        if (headless_)
        {
            return CGSizeMake(headless_width_, headless_height_);
        }
        return view_.drawableSize;
    }

    [[nodiscard]] MetalRHI *GetRHI() const
    {
        return rhi_;
    }

    [[nodiscard]] bool IsHeadless() const
    {
        return headless_;
    }

    void CreateBackBuffer();

    void SwapBuffer();

    void BeginFrame();

    void EndFrame();

    void BeginFrameCapture();

    void EndFrameCapture();

    void BeginCommandBuffer();

    void SubmitCommandBuffer();

    void WaitUntilDeviceIdle();

    void CaptureNextFrames(int count);

private:
    id<MTLDevice> device_;
    id<MTLCommandQueue> command_queue_;
    MetalView *view_;
    MetalRHI *rhi_;
    id<CAMetalDrawable> current_drawable_;
    MetalCommandContext command_context_;
    id<MTLCommandBuffer> last_command_buffer_;

    dispatch_semaphore_t frame_throttle_semaphore_ = nullptr;

    // TODO(tqjxlm): remove the reference here
    RHIResourceRef<MetalImage> back_buffer_color_;

    uint32_t num_frames_to_capture_ = 0;
    bool is_capturing_frame_ = false;
    bool headless_ = false;
    uint32_t headless_width_ = 0;
    uint32_t headless_height_ = 0;
};

// this static member is used to simplify code structure
// its lift cycle is managed by MetalRHI
inline std::unique_ptr<MetalContext> context;
} // namespace sparkle

#endif
