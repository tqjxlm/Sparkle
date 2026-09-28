#pragma once

#include "renderer/pass/PipelinePass.h"

#include "rhi/RHIPIpelineState.h"

namespace sparkle
{
class RGRasterContext;

class MeshPass : public PipelinePass
{
public:
    // what the passes sample every texture of a material with: one sampler for the whole material, so its LOD range
    // cannot derive from any single texture's mip count
    static constexpr RHISampler::SamplerAttribute MaterialTextureSampler{
        .address_mode = RHISampler::SamplerAddressMode::Repeat,
        .filtering_method_min = RHISampler::FilteringMethod::Linear,
        .filtering_method_mag = RHISampler::FilteringMethod::Linear,
        .filtering_method_mipmap = RHISampler::FilteringMethod::Linear,
        .max_lod = RHISampler::SamplerAttribute::UnclampedLod};

    MeshPass(RHIContext *ctx, SceneRenderProxy *scene_proxy) : PipelinePass(ctx), scene_proxy_(scene_proxy)
    {
    }

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

    virtual void HandleNewPrimitive(uint32_t primitive_id) = 0;

    virtual void HandleUpdatedPrimitive(uint32_t primitive_id) = 0;

    virtual void HandleRemovedPrimitive(uint32_t primitive_id);

    virtual void HandleMovedPrimitive(uint32_t from, uint32_t to);

protected:
    void DrawPrimitives(RGRasterContext &context) const;

    SceneRenderProxy *scene_proxy_;

    std::vector<RHIResourceRef<RHIPipelineState>> pipeline_states_;

    bool initialized_ = false;
};
} // namespace sparkle
