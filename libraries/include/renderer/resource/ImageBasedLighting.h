#pragma once

#include "core/Event.h"
#include "renderer/pass/IBLPass.h"

#include <cstdint>

namespace sparkle
{
class ImageBasedLighting
{
public:
    explicit ImageBasedLighting(const RHIResourceRef<RHIImage> &env_map);

    ~ImageBasedLighting();

    void InitRenderResources(RHIContext *ctx, const RenderConfig &config);

    [[nodiscard]] bool NeedUpdate() const
    {
        return (ibl_diffuse_pass_ && !ibl_diffuse_pass_->IsReady()) ||
               (ibl_specular_pass_ && !ibl_specular_pass_->IsReady()) || (ibl_brdf_pass_ && !ibl_brdf_pass_->IsReady());
    }

    void CookOnTheFly(const RenderConfig &config);

    [[nodiscard]] RHIResourceRef<RHIImage> GetDiffuseMap() const
    {
        return ibl_diffuse_pass_ ? ibl_diffuse_pass_->GetResource() : nullptr;
    }

    [[nodiscard]] RHIResourceRef<RHIImage> GetSpecularMap() const
    {
        return ibl_specular_pass_ ? ibl_specular_pass_->GetResource() : nullptr;
    }

    [[nodiscard]] RHIResourceRef<RHIImage> GetBRDFMap() const
    {
        return ibl_brdf_pass_ ? ibl_brdf_pass_->GetResource() : nullptr;
    }

    auto &OnRenderResourceChange()
    {
        return render_resource_change_event_.OnTrigger();
    }

private:
    [[nodiscard]] unsigned GetAdaptiveCookStepBudget(const RenderConfig &config);

    RHIResourceRef<RHIImage> env_map_;

    std::unique_ptr<IBLPass> ibl_diffuse_pass_;
    std::unique_ptr<IBLPass> ibl_specular_pass_;
    std::unique_ptr<IBLPass> ibl_brdf_pass_;

    RHIContext *rhi_ = nullptr;
    unsigned cook_steps_per_frame_ = 2;
    uint8_t next_cook_pass_index_ = 0;

    Event render_resource_change_event_;
};
}; // namespace sparkle
