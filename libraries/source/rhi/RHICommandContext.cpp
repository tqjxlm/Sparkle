#include "rhi/RHICommandContext.h"

#include "rhi/RHI.h"

namespace sparkle
{
void RHICommandContext::BeginRenderPass(const RHIResourceRef<RHIRenderPass> &pass)
{
    // swap chain recreation replaces the back buffer render target
    if (pass->TargetsBackBuffer())
    {
        pass->SetRenderTarget(rhi_->GetBackBufferRenderTarget());
    }

    const auto info = pass->GetRenderingInfo();
    BeginRendering(info, pass->GetName(), pass->SelectTimer(), RHIRenderPass::TrackBeginTransitions(info));

    current_render_pass_ = pass;
}

void RHICommandContext::EndRenderPass()
{
    ASSERT_F(current_render_pass_ != nullptr, "No active render pass!");

    const auto pass = current_render_pass_;
    current_render_pass_ = nullptr;

    EndRendering(pass->TrackEndTransitions(rendering_info_));
}

void RHICommandContext::BeginRendering(const RHIRenderingInfo &info, const std::string &name, RHITimer *timer,
                                       std::span<const RHIImageBarrier> barriers)
{
    ASSERT_F(!rendering_, "Previous render pass not ended {}", rendering_name_);
    ASSERT_F(current_compute_pass_ == nullptr, "Previous compute pass not ended {}", current_compute_pass_->GetName());

    if (timer)
    {
        timer->Begin(*this);
    }
    BeginDebugLabel(name);

    Barrier(barriers, {});

    rendering_info_ = info;
    attachment_signature_ = info.GetSignature();
    rendering_name_ = name;
    rendering_timer_ = timer;

    BeginRenderingInternal(info, name, timer);

    rendering_ = true;
}

void RHICommandContext::EndRendering(std::span<const RHIImageBarrier> barriers)
{
    ASSERT_F(rendering_, "No active render pass!");
    ASSERT_F(current_render_pass_ == nullptr, "Render pass {} must end with EndRenderPass", rendering_name_);

    rendering_ = false;

    EndRenderingInternal();

    Barrier(barriers, {});

    EndDebugLabel();
    if (rendering_timer_)
    {
        rendering_timer_->End(*this);
        rendering_timer_ = nullptr;
    }
}

void RHICommandContext::BeginComputePass(const RHIResourceRef<RHIComputePass> &pass)
{
    ASSERT_F(current_compute_pass_ == nullptr, "Previous compute pass not ended {}", current_compute_pass_->GetName());
    ASSERT_F(!rendering_, "Previous render pass not ended {}", rendering_name_);

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

    ASSERT_F(!rendering_, "Barrier inside render pass {}", rendering_name_);

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
    ASSERT_F(!rendering_, "{} inside render pass {}", command, rendering_name_);
    ASSERT_F(current_compute_pass_ == nullptr, "{} inside compute pass {}", command, current_compute_pass_->GetName());
}
} // namespace sparkle
