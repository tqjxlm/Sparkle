#pragma once

#include "rhi/RHIResource.h"

#include "rhi/RHIImage.h"
#include "rhi/RHIRenderTarget.h"
#include "rhi/RHIRenderingInfo.h"

namespace sparkle
{
class RHIRenderPass : public RHIResource
{
public:
    using LoadOp = RHILoadOp;
    using StoreOp = RHIStoreOp;

    struct Attribute
    {
        LoadOp color_load_op = RHIRenderPass::LoadOp::None;
        StoreOp color_store_op = RHIRenderPass::StoreOp::Store;
        RHIImageLayout color_final_layout = RHIImageLayout::ColorOutput;
        Vector4 clear_color{0, 0, 0, 1};

        LoadOp depth_load_op = RHIRenderPass::LoadOp::None;
        StoreOp depth_store_op = RHIRenderPass::StoreOp::None;
        RHIImageLayout depth_final_layout = RHIImageLayout::DepthStencilOutput;
    };

    RHIRenderPass(Attribute attribute, const RHIResourceRef<RHIRenderTarget> &rt, const std::string &name)
        : RHIResource(name), attribute_(std::move(attribute)), render_target_(rt),
          targets_back_buffer_(rt->IsBackBufferTarget())
    {
    }

    [[nodiscard]] auto *GetRenderTarget() const
    {
        return render_target_.expired() ? nullptr : render_target_.lock().get();
    }

    void SetRenderTarget(const RHIResourceRef<RHIRenderTarget> &rt)
    {
        render_target_ = rt;
    }

    [[nodiscard]] bool TargetsBackBuffer() const
    {
        return targets_back_buffer_;
    }

    // describes this pass over the render target's current images
    [[nodiscard]] RHIRenderingInfo GetRenderingInfo() const;

    // the rendering info taken when the pass began, valid until it ends
    [[nodiscard]] const RHIRenderingInfo &GetActiveRenderingInfo() const
    {
        return active_rendering_info_;
    }

    [[nodiscard]] const RHIAttachmentSignature &GetActiveAttachmentSignature() const
    {
        return active_attachment_signature_;
    }

protected:
    Attribute attribute_;
    RHIResourceWeakRef<RHIRenderTarget> render_target_;

private:
    friend class RHIContext;

    void CaptureRenderingInfo()
    {
        active_rendering_info_ = GetRenderingInfo();
        active_attachment_signature_ = active_rendering_info_.GetSignature();
    }

    bool targets_back_buffer_;
    RHIRenderingInfo active_rendering_info_;
    RHIAttachmentSignature active_attachment_signature_;
};
} // namespace sparkle
