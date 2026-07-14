#pragma once

#include "core/Event.h"
#include "rhi/RHIImage.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace sparkle
{
class CookHandle;
class CookJob;
class IBLPass;
class Image2DCube;
struct RenderConfig;

class ImageBasedLighting
{
public:
    ImageBasedLighting(const RHIResourceRef<RHIImage> &env_map, std::shared_ptr<const Image2DCube> env_map_cpu);

    ~ImageBasedLighting();

    void InitRenderResources(RHIContext *ctx, const RenderConfig &config);

    [[nodiscard]] bool NeedUpdate() const;

    void CookOnTheFly(const RenderConfig &config);

    [[nodiscard]] RHIResourceRef<RHIImage> GetDiffuseMap() const;

    [[nodiscard]] RHIResourceRef<RHIImage> GetSpecularMap() const;

    [[nodiscard]] RHIResourceRef<RHIImage> GetBRDFMap() const;

    auto &OnRenderResourceChange()
    {
        return render_resource_change_event_.OnTrigger();
    }

private:
    [[nodiscard]] unsigned GetAdaptiveCookStepBudget(const RenderConfig &config);

    void RequestCpuCook(std::unique_ptr<CookJob> brdf_job, std::unique_ptr<CookJob> diffuse_job,
                        std::unique_ptr<CookJob> specular_job);

    RHIResourceRef<RHIImage> env_map_;

    std::shared_ptr<const Image2DCube> env_map_cpu_;

    std::vector<CookHandle> cook_handles_;

    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    bool cpu_cook_pending_ = false;

    std::unique_ptr<IBLPass> ibl_diffuse_pass_;
    std::unique_ptr<IBLPass> ibl_specular_pass_;
    std::unique_ptr<IBLPass> ibl_brdf_pass_;

    RHIContext *rhi_ = nullptr;
    unsigned cook_steps_per_frame_ = 2;
    uint8_t next_cook_pass_index_ = 0;

    Event render_resource_change_event_;
};
} // namespace sparkle
