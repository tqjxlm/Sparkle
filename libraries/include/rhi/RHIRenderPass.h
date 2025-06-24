#pragma once

#include "rhi/RHIResource.h"

#include "rhi/RHIImage.h"
#include "rhi/RHIRenderTarget.h"

namespace sparkle
{
class RHIRenderPass : public RHIResource
{
public:
    enum class LoadOp : uint8_t
    {
        None,
        Load,
        Clear,
    };

    enum class StoreOp : uint8_t
    {
        None,
        Store,
    };

    struct Attribute
    {
        LoadOp color_load_op = RHIRenderPass::LoadOp::None;
        StoreOp color_store_op = RHIRenderPass::StoreOp::Store;
        RHIImageLayout color_initial_layout = RHIImageLayout::Undefined;
        RHIImageLayout color_final_layout = RHIImageLayout::ColorOutput;
        Vector4 clear_color{0, 0, 0, 1};

        LoadOp depth_load_op = RHIRenderPass::LoadOp::None;
        StoreOp depth_store_op = RHIRenderPass::StoreOp::None;
        RHIImageLayout depth_initial_layout = RHIImageLayout::Undefined;
        RHIImageLayout depth_final_layout = RHIImageLayout::DepthStencilOutput;
    };

    RHIRenderPass(Attribute attribute, const RHIResourceRef<RHIRenderTarget> &rt, const std::string &name)
        : RHIResource(name), attribute_(std::move(attribute)), render_target_(rt)
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

protected:
    Attribute attribute_;
    RHIResourceWeakRef<RHIRenderTarget> render_target_;
};
} // namespace sparkle
