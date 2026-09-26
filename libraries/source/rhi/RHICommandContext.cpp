#include "rhi/RHICommandContext.h"

#include "rhi/RHI.h"

namespace sparkle
{
void RHICommandContext::BeginRenderPass(const RHIResourceRef<RHIRenderPass> &pass)
{
    ASSERT_F(current_render_pass_ == nullptr, "Previous render pass not ended {}", current_render_pass_->GetName());
    ASSERT_F(current_compute_pass_ == nullptr, "Previous compute pass not ended {}", current_compute_pass_->GetName());

    // swap chain recreation replaces the back buffer render target
    if (pass->TargetsBackBuffer())
    {
        pass->SetRenderTarget(rhi_->GetBackBufferRenderTarget());
    }

    pass->CaptureRenderingInfo();

    pass->BeginTimer(*this);

    // the internal begin records the attachment barriers before the pass opens
    BeginRenderPassInternal(pass);

    current_render_pass_ = pass;
}

void RHICommandContext::EndRenderPass()
{
    ASSERT_F(current_render_pass_ != nullptr, "No active render pass!");

    // the internal end records barriers after the rendering ends, so the pass is no longer current
    const auto pass = current_render_pass_;
    current_render_pass_ = nullptr;

    EndRenderPassInternal(pass);

    pass->EndTimer(*this);
}

void RHICommandContext::BeginComputePass(const RHIResourceRef<RHIComputePass> &pass)
{
    ASSERT_F(current_compute_pass_ == nullptr, "Previous compute pass not ended {}", current_compute_pass_->GetName());
    ASSERT_F(current_render_pass_ == nullptr, "Previous render pass not ended {}", current_render_pass_->GetName());

    pass->BeginTimer(*this);

    current_compute_pass_ = pass;

    BeginComputePassInternal(pass);
}

void RHICommandContext::EndComputePass(const RHIResourceRef<RHIComputePass> &pass)
{
    ASSERT(current_compute_pass_ == pass);

    current_compute_pass_ = nullptr;

    EndComputePassInternal(pass);

    pass->EndTimer(*this);
}

void RHICommandContext::Barrier(std::span<const RHIImageBarrier> image_barriers,
                                std::span<const RHIMemoryBarrier> memory_barriers)
{
    if (image_barriers.empty() && memory_barriers.empty())
    {
        return;
    }

    ASSERT_F(current_render_pass_ == nullptr, "Barrier inside render pass {}", current_render_pass_->GetName());

    BarrierInternal(image_barriers, memory_barriers);
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
    ASSERT_F(current_render_pass_ == nullptr, "{} inside render pass {}", command, current_render_pass_->GetName());
    ASSERT_F(current_compute_pass_ == nullptr, "{} inside compute pass {}", command, current_compute_pass_->GetName());
}
} // namespace sparkle
