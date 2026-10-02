#include "rhi/RHICommandContext.h"

#include "rhi/RHI.h"

namespace sparkle
{
static void AssertAttachmentLayout([[maybe_unused]] const std::string &pass_name, const RHIImage *image,
                                   unsigned mip_level, unsigned array_layer, RHIImageLayout layout)
{
    ASSERT_F(image->GetCurrentLayout(mip_level, array_layer) == layout,
             "render pass {} begins with attachment {} (mip {}, layer {}) outside its attachment layout", pass_name,
             image->GetName(), mip_level, array_layer);
}

void RHICommandContext::BeginRendering(const RHIRenderingInfo &info, const std::string &name, RHIPass *timed_pass,
                                       std::span<const RHIImageBarrier> barriers,
                                       std::span<const RHIMemoryBarrier> memory_barriers)
{
    ASSERT_F(!rendering_, "Previous render pass not ended {}", rendering_name_);
    ASSERT_F(current_compute_pass_ == nullptr, "Previous compute pass not ended {}", current_compute_pass_->GetName());

    auto *timer = timed_pass ? timed_pass->SelectTimer() : nullptr;
    if (timer)
    {
        timer->Begin(*this);
    }
    BeginDebugLabel(name);

    Barrier(barriers, memory_barriers);

    for (const auto &attachment : info.color_attachments)
    {
        if (attachment.image)
        {
            AssertAttachmentLayout(name, attachment.image, attachment.mip_level, attachment.array_layer,
                                   attachment.layout);
        }
    }
    if (const auto &depth = info.depth_attachment; depth.image)
    {
        AssertAttachmentLayout(name, depth.image, depth.mip_level, depth.array_layer,
                               RHIImageLayout::DepthStencilOutput);
    }

    rendering_info_ = info;
    attachment_signature_ = info.GetSignature();
    rendering_name_ = name;
    rendering_timer_ = timer;

    BeginRenderingInternal(info, name, timer);

    rendering_ = true;
}

void RHICommandContext::EndRendering()
{
    ASSERT_F(rendering_, "No active render pass!");

    rendering_ = false;

    EndRenderingInternal();

    EndDebugLabel();
    if (rendering_timer_)
    {
        rendering_timer_->End(*this);
        rendering_timer_ = nullptr;
    }
}

void RHICommandContext::SetUnusedAttachments(uint8_t unwritten_color_slots, bool depth_unused)
{
    ASSERT_F(rendering_, "SetUnusedAttachments outside a render pass");

    attachment_signature_ = rendering_info_.GetSignature();
    attachment_signature_.unwritten_color_slots = unwritten_color_slots;
    attachment_signature_.depth_unused = depth_unused;
}

void RHICommandContext::BeginComputePass(const RHIResourceRef<RHIComputePass> &pass,
                                         std::span<const RHIImageBarrier> barriers,
                                         std::span<const RHIMemoryBarrier> memory_barriers)
{
    ASSERT_F(current_compute_pass_ == nullptr, "Previous compute pass not ended {}", current_compute_pass_->GetName());
    ASSERT_F(!rendering_, "Previous render pass not ended {}", rendering_name_);

    pass->BeginTimer(*this);
    BeginDebugLabel(pass->GetName());

    Barrier(barriers, memory_barriers);

    current_compute_pass_ = pass;

    BeginComputePassInternal(pass);
}

void RHICommandContext::EndComputePass(const RHIResourceRef<RHIComputePass> &pass)
{
    ASSERT(current_compute_pass_ == pass);

    current_compute_pass_ = nullptr;

    EndComputePassInternal(pass);

    EndDebugLabel();
    pass->EndTimer(*this);
}

void RHICommandContext::Barrier(std::span<const RHIImageBarrier> image_barriers,
                                std::span<const RHIMemoryBarrier> memory_barriers)
{
    if (image_barriers.empty() && memory_barriers.empty())
    {
        return;
    }

    ASSERT_F(!rendering_, "Barrier inside render pass {}", rendering_name_);

    BarrierInternal(image_barriers, memory_barriers);
}

void RHICommandContext::PixelLocalBarrier(std::span<const RHIImageBarrier> image_barriers)
{
    if (image_barriers.empty())
    {
        return;
    }

    ASSERT_F(rendering_, "PixelLocalBarrier outside a render pass");

    PixelLocalBarrierInternal(image_barriers);
}

void RHICommandContext::DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args)
{
    ASSERT_F(rendering_, "DrawMesh outside a render pass");

    if (!pipeline_state)
    {
        return;
    }

    ApplyBindings(*pipeline_state);
    DrawMeshInternal(pipeline_state, draw_args);
}

void RHICommandContext::DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                                        Vector3UInt thread_per_group)
{
    ASSERT_F(current_compute_pass_ != nullptr, "DispatchCompute outside a compute pass");

    ApplyBindings(*pipeline);
    DispatchComputeInternal(pipeline, total_threads, thread_per_group);
}

void RHICommandContext::ApplyBindings(RHIPipelineState &pipeline)
{
    if (pipelines_.empty() || pipelines_.back() != &pipeline)
    {
        pipelines_.push_back(&pipeline);
    }
    for (auto index = 0u; index < bindings_.size(); index++)
    {
        if (pipeline.ApplyBinding(bindings_[index]))
        {
            bindings_applied_[index] = true;
        }
    }
}

void RHICommandContext::CopyBuffer(const RHIBuffer *src, const RHIBuffer *dst)
{
    AssertOutsidePass("CopyBuffer");
    CopyBufferInternal(src, dst);
}

void RHICommandContext::CopyBufferToImage(const RHIBuffer *src, const RHIImage *dst)
{
    AssertOutsidePass("CopyBufferToImage");
    CopyBufferToImageInternal(src, dst);
}

void RHICommandContext::CopyImageToBuffer(const RHIImage *src, const RHIBuffer *dst)
{
    AssertOutsidePass("CopyImageToBuffer");
    CopyImageToBufferInternal(src, dst);
}

void RHICommandContext::BlitImage(const RHIImage *src, const RHIImage *dst, RHISampler::FilteringMethod filter)
{
    AssertOutsidePass("BlitImage");
    BlitImageInternal(src, dst, filter);
}

void RHICommandContext::AssertOutsidePass([[maybe_unused]] std::string_view command) const
{
    ASSERT_F(!rendering_, "{} inside render pass {}", command, rendering_name_);
    ASSERT_F(current_compute_pass_ == nullptr, "{} inside compute pass {}", command, current_compute_pass_->GetName());
}
} // namespace sparkle
