#pragma once

#include "renderer/pass/PipelinePass.h"

#include "rhi/RHIPIpelineState.h"

namespace sparkle
{
class MeshPass : public PipelinePass
{
public:
    MeshPass(RHIContext *ctx, SceneRenderProxy *scene_proxy) : PipelinePass(ctx), scene_proxy_(scene_proxy)
    {
    }

    void Render() override;

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

    virtual void HandleNewPrimitive(uint32_t primitive_id) = 0;

    virtual void HandleUpdatedPrimitive(uint32_t primitive_id) = 0;

    virtual void HandleRemovedPrimitive(uint32_t primitive_id);

    virtual void HandleMovedPrimitive(uint32_t from, uint32_t to);

protected:
    SceneRenderProxy *scene_proxy_;

    std::vector<RHIResourceRef<RHIPipelineState>> pipeline_states_;

    bool initialized_ = false;
};
} // namespace sparkle
