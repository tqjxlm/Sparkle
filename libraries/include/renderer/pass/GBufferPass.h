#pragma once

#include "renderer/pass/MeshPass.h"

#include "rhi/RHIRenderTarget.h"

namespace sparkle
{
class MeshRenderProxy;

class GBufferPass : public MeshPass
{
public:
    GBufferPass(RHIContext *ctx, SceneRenderProxy *scene_proxy, RHIRenderTarget::ColorImageArray gbuffer_images,
                const RHIResourceRef<RHIImage> &scene_depth);

    void InitRenderResources(const RenderConfig &config) override;

    void HandleNewPrimitive(uint32_t primitive_id) override;

    void HandleUpdatedPrimitive(uint32_t primitive_id) override;

    void Render() override;

private:
    static void SetupVertices(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy);
    void SetupVertexShader(const RHIResourceRef<RHIPipelineState> &pso) const;
    void SetupPixelShader(const RHIResourceRef<RHIPipelineState> &pso) const;
    void BindShaderResources(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy) const;

    RHIResourceRef<RHIImage> scene_depth_;

    RHIResourceRef<RHIRenderTarget> render_target_;

    RHIResourceRef<RHIRenderPass> pass_;

    RHIRenderTarget::ColorImageArray gbuffer_images_;
};
} // namespace sparkle
