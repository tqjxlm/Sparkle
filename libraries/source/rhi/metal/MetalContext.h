#pragma once

#if FRAMEWORK_APPLE

#include "MetalImage.h"
#include "MetalRHIInternal.h"
#import "apple/MetalView.h"

namespace sparkle
{
class MetalContext
{
public:
    MetalContext(MetalRHI *context, MetalView *mtk_view);

    [[nodiscard]] id<MTLDevice> GetDevice() const
    {
        return device_;
    }

    [[nodiscard]] id<MTLCommandBuffer> GetCurrentCommandBuffer() const
    {
        return current_command_buffer_;
    }

    [[nodiscard]] RHIResourceRef<MetalImage> GetBackBufferColor() const
    {
        return back_buffer_color_;
    }

    [[nodiscard]] bool IsInCommandBuffer() const
    {
        return current_command_buffer_ != nullptr;
    }

    [[nodiscard]] MetalView *GetView() const
    {
        return view_;
    }

    [[nodiscard]] MetalRHI *GetRHI() const
    {
        return rhi_;
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
    id<MTLCommandBuffer> current_command_buffer_;
    id<MTLCommandBuffer> last_command_buffer_;

    // TODO(tqjxlm): remove the reference here
    RHIResourceRef<MetalImage> back_buffer_color_;

    uint32_t num_frames_to_capture_ = 0;
    bool is_capturing_frame_ = false;
};

// this static member is used to simplify code structure
// its lift cycle is managed by MetalRHI
inline std::unique_ptr<MetalContext> context;
} // namespace sparkle

#endif
