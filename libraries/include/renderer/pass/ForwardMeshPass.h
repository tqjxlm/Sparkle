#pragma once

#include "renderer/pass/MeshPass.h"

namespace sparkle
{
class MeshRenderProxy;
class RenderGraph;
struct LightingInputs;
struct RGTexture;

class ForwardMeshPass : public MeshPass
{
public:
    // draws into a color attachment of `color_format` at slot 0 and a depth attachment of `depth_format`
    ForwardMeshPass(RHIContext *ctx, SceneRenderProxy *scene_proxy, PixelFormat color_format, PixelFormat depth_format);

    void InitRenderResources(const RenderConfig &config) override;

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

    void HandleNewPrimitive(uint32_t primitive_id) override;

    void HandleUpdatedPrimitive(uint32_t primitive_id) override;

    // adds a Raster pass shading the scene, lit by `lighting`, into `scene_color` and `scene_depth`, both cleared
    void AddTo(RenderGraph &graph, const LightingInputs &lighting, RGTexture scene_color, RGTexture scene_depth) const;

private:
    static void SetupVertices(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy);
    void SetupVertexShader(const RHIResourceRef<RHIPipelineState> &pso) const;
    void SetupPixelShader(const RHIResourceRef<RHIPipelineState> &pso) const;
    void BindPassResources(const RHIResourceRef<RHIPipelineState> &pso) const;

    RHIAttachmentSignature signature_;

    RHIResourceRef<RHIBuffer> uniform_buffer_;
};
} // namespace sparkle
