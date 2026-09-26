#pragma once

#include "rhi/RHIPass.h"

#include "rhi/RHIImage.h"
#include "rhi/RHIRenderTarget.h"
#include "rhi/RHIRenderingInfo.h"

namespace sparkle
{
class RHIRenderPass : public RHIPass
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

        // measures the pass's GPU time, see RHIPass
        bool need_timestamp = false;
    };

    RHIRenderPass(RHIContext *rhi, Attribute attribute, const RHIResourceRef<RHIRenderTarget> &rt,
                  const std::string &name)
        : RHIPass(rhi, attribute.need_timestamp, name), attribute_(std::move(attribute)), render_target_(rt),
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

protected:
    Attribute attribute_;
    RHIResourceWeakRef<RHIRenderTarget> render_target_;

private:
    friend class RHICommandContext;

    // tracks the attachments into their attachment layouts, discarding the contents the pass does not load, and returns
    // the barriers
    [[nodiscard]] static std::vector<RHIImageBarrier> TrackBeginTransitions(const RHIRenderingInfo &info);

    // tracks the attachments into the pass's final layouts and returns the barriers
    [[nodiscard]] std::vector<RHIImageBarrier> TrackEndTransitions(const RHIRenderingInfo &info) const;

    bool targets_back_buffer_;
};
} // namespace sparkle
