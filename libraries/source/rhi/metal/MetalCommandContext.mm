#if FRAMEWORK_APPLE

#include "MetalCommandContext.h"

#include "MetalBuffer.h"
#include "MetalComputePass.h"
#include "MetalImage.h"
#include "MetalPipelineState.h"
#include "MetalRenderPass.h"

namespace sparkle
{
static MTLPrimitiveType GetMetalPrimitiveType(RHIPipelineState::PolygonMode mode)
{
    switch (mode)
    {
    case RHIPipelineState::PolygonMode::Fill:
        return MTLPrimitiveTypeTriangle;
    case RHIPipelineState::PolygonMode::Line:
        return MTLPrimitiveTypeLine;
    case RHIPipelineState::PolygonMode::Point:
        return MTLPrimitiveTypePoint;
    default:
        UnImplemented(mode);
    }
}

void MetalCommandContext::Begin(id<MTLCommandBuffer> command_buffer)
{
    ASSERT(!command_buffer_);

    command_buffer_ = command_buffer;
}

void MetalCommandContext::End()
{
    AssertOutsidePass("End of command buffer");

    command_buffer_ = nil;
}

void MetalCommandContext::DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args)
{
    ASSERT(render_encoder_);

    auto *pso = RHICast<MetalGraphicsPipeline>(pipeline_state);
    const auto *pass = RHICast<MetalRenderPass>(GetCurrentRenderPass());

    auto index_buffer = RHICast<MetalBuffer>(pso->GetIndexBuffer())->GetResource();

    pso->Bind(render_encoder_, pass->GetActiveAttachmentSignature());

    [render_encoder_ drawIndexedPrimitives:GetMetalPrimitiveType(pipeline_state->GetRasterizationState().polygon_mode)
                                indexCount:draw_args.index_count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:index_buffer
                         indexBufferOffset:draw_args.first_index
                             instanceCount:draw_args.instance_count
                                baseVertex:draw_args.first_vertex
                              baseInstance:draw_args.first_instance];
}

void MetalCommandContext::DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                                          Vector3UInt thread_per_group)
{
    ASSERT(compute_encoder_);

    auto *pso = RHICast<MetalComputePipeline>(pipeline);

    pso->Bind(compute_encoder_);

    MTLSize grid_size = MTLSizeMake(total_threads.x(), total_threads.y(), total_threads.z());
    MTLSize threadgroup_size = MTLSizeMake(thread_per_group.x(), thread_per_group.y(), thread_per_group.z());

    [compute_encoder_ dispatchThreads:grid_size threadsPerThreadgroup:threadgroup_size];
}

// each blit encoder lives for one command only; the callers assert that no pass encoder is open
void MetalCommandContext::CopyBufferInternal(const RHIBuffer *src, const RHIBuffer *dst)
{
    auto encoder = [command_buffer_ blitCommandEncoder];
    RHICast<MetalBuffer>(src)->CopyToBuffer(encoder, dst);
    [encoder endEncoding];
}

void MetalCommandContext::CopyBufferToImageInternal(const RHIBuffer *src, const RHIImage *dst)
{
    auto encoder = [command_buffer_ blitCommandEncoder];
    RHICast<MetalBuffer>(src)->CopyToImage(encoder, dst);
    [encoder endEncoding];
}

void MetalCommandContext::CopyImageToBufferInternal(const RHIImage *src, const RHIBuffer *dst)
{
    auto encoder = [command_buffer_ blitCommandEncoder];
    RHICast<MetalImage>(src)->CopyToBuffer(encoder, dst);
    [encoder endEncoding];
}

// MPS encodes its own compute encoders on the command buffer
void MetalCommandContext::BlitImageInternal(const RHIImage *src, const RHIImage *dst, RHISampler::FilteringMethod)
{
    RHICast<MetalImage>(src)->BlitToImage(command_buffer_, dst);
}

void MetalCommandContext::BeginRenderPassInternal(const RHIResourceRef<RHIRenderPass> &pass)
{
    render_encoder_ = RHICast<MetalRenderPass>(pass)->Begin(command_buffer_);
}

void MetalCommandContext::EndRenderPassInternal(const RHIResourceRef<RHIRenderPass> & /*pass*/)
{
    [render_encoder_ endEncoding];
    render_encoder_ = nil;
}

void MetalCommandContext::BeginComputePassInternal(const RHIResourceRef<RHIComputePass> &pass)
{
    compute_encoder_ = RHICast<MetalComputePass>(pass)->Begin(command_buffer_);
}

void MetalCommandContext::EndComputePassInternal(const RHIResourceRef<RHIComputePass> & /*pass*/)
{
    [compute_encoder_ endEncoding];
    compute_encoder_ = nil;
}
} // namespace sparkle

#endif
