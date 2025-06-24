#pragma once

#include "renderer/pass/MeshPass.h"

#include "core/Event.h"
#include "rhi/RHIRayTracing.h"

namespace sparkle
{
class ImageBasedLighting;
class MeshRenderProxy;

class ForwardMeshPass : public MeshPass
{
public:
    struct PassResources
    {
        RHIResourceRef<RHIImage> scene_color;
        RHIResourceRef<RHIImage> scene_depth;
        RHIResourceRef<RHIImage> prepass_depth_map = nullptr;
        RHIResourceRef<RHITLAS> tlas = nullptr;
    };

    ForwardMeshPass(RHIContext *ctx, SceneRenderProxy *scene_proxy, PassResources resources);

    void InitRenderResources(const RenderConfig &config) override;

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

    void HandleNewPrimitive(uint32_t primitive_id) override;

    void HandleUpdatedPrimitive(uint32_t primitive_id) override;

    void SetDirectionalShadow(const RHIResourceRef<RHIImage> &shadow_map);

    void SetIBL(ImageBasedLighting *ibl);

    void RebindAllShaderResources();

    void Render() override;

private:
    static void SetupVertices(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy,
                              bool use_prepass);
    void SetupVertexShader(const RHIResourceRef<RHIPipelineState> &pso) const;
    void SetupPixelShader(const RHIResourceRef<RHIPipelineState> &pso) const;
    void BindPassResources(const RHIResourceRef<RHIPipelineState> &pso) const;

    RHIResourceRef<RHIBuffer> uniform_buffer_;
    RHIResourceRef<RHIRenderPass> base_pass_;
    RHIResourceRef<RHIRenderTarget> render_target_;

    RHIResourceRef<RHIImage> shadow_map_;
    ImageBasedLighting *ibl_ = nullptr;

    bool ibl_dirty_ = false;
    std::unique_ptr<EventSubscription> ibl_changed_subscription_;

    PassResources resources_;

    bool use_prepass_;
    bool use_ray_tracing_;
};
} // namespace sparkle
