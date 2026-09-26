#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

namespace sparkle
{
class MetalCommandContext final : public RHICommandContext
{
public:
    using RHICommandContext::RHICommandContext;

    [[nodiscard]] id<MTLCommandBuffer> GetCommandBuffer() const
    {
        return command_buffer_;
    }

    // the encoder of the open render pass
    [[nodiscard]] id<MTLRenderCommandEncoder> GetRenderEncoder() const
    {
        return render_encoder_;
    }

    // the encoder of the open compute pass
    [[nodiscard]] id<MTLComputeCommandEncoder> GetComputeEncoder() const
    {
        return compute_encoder_;
    }

    void Begin(id<MTLCommandBuffer> command_buffer);

    void End();

    void DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args) override;

    void DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                         Vector3UInt thread_per_group) override;

protected:
    void BarrierInternal(std::span<const RHIImageBarrier> /*image_barriers*/,
                         std::span<const RHIMemoryBarrier> /*memory_barriers*/) override
    {
    }

    void CopyBufferInternal(const RHIBuffer *src, const RHIBuffer *dst) override;
    void CopyBufferToImageInternal(const RHIBuffer *src, const RHIImage *dst) override;
    void CopyImageToBufferInternal(const RHIImage *src, const RHIBuffer *dst) override;
    void BlitImageInternal(const RHIImage *src, const RHIImage *dst, RHISampler::FilteringMethod filter) override;
    void BeginRenderPassInternal(const RHIResourceRef<RHIRenderPass> &pass) override;
    void EndRenderPassInternal(const RHIResourceRef<RHIRenderPass> &pass) override;
    void BeginComputePassInternal(const RHIResourceRef<RHIComputePass> &pass) override;
    void EndComputePassInternal(const RHIResourceRef<RHIComputePass> &pass) override;

private:
    id<MTLCommandBuffer> command_buffer_ = nil;
    id<MTLRenderCommandEncoder> render_encoder_ = nil;
    id<MTLComputeCommandEncoder> compute_encoder_ = nil;
};
} // namespace sparkle

#endif
