#pragma once

#include "renderer/pass/MeshPass.h"

#include "renderer/graph/RenderGraph.h"

namespace sparkle
{
class MeshRenderProxy;

class GBufferPass : public MeshPass
{
public:
    // the scene's surfaces, packed into one texel per pixel
    static constexpr RGTextureDesc PackedDesc{.format = PixelFormat::RGBAUInt32, .size_class = RGSizeClass::Scene};

    // draws into a packed GBuffer at slot 0 and a depth attachment of `depth_format`
    GBufferPass(RHIContext *ctx, SceneRenderProxy *scene_proxy, PixelFormat depth_format);

    void InitRenderResources(const RenderConfig &config) override;

    void HandleNewPrimitive(uint32_t primitive_id) override;

    void HandleUpdatedPrimitive(uint32_t primitive_id) override;

    // adds a Raster pass drawing into a new GBufferPacked and `scene_depth`, both cleared, and returns the GBuffer
    [[nodiscard]] RGTexture AddTo(RenderGraph &graph, RGTexture scene_depth) const;

private:
    static void SetupVertices(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy);
    void SetupVertexShader(const RHIResourceRef<RHIPipelineState> &pso) const;
    void SetupPixelShader(const RHIResourceRef<RHIPipelineState> &pso) const;
    void BindShaderResources(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy) const;

    RHIAttachmentSignature signature_;
};
} // namespace sparkle
