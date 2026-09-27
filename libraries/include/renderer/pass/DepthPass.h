#pragma once

#include "renderer/pass/MeshPass.h"

#include "core/math/Types.h"
#include "renderer/graph/RenderGraph.h"
#include "rhi/RHIBuffer.h"

namespace sparkle
{
// draws the scene's depth as the directional light sees it into a shadow map
class DepthPass : public MeshPass
{
public:
    DepthPass(RHIContext *ctx, SceneRenderProxy *scene_proxy, unsigned width, unsigned height);

    void InitRenderResources(const RenderConfig &config) override;

    void HandleNewPrimitive(uint32_t primitive_id) override;

    void HandleUpdatedPrimitive([[maybe_unused]] uint32_t primitive_id) override
    {
    }

    void SetProjectionMatrix(const Mat4 &matrix);

    // adds a Raster pass drawing into a new ShadowMap, cleared, and returns it
    [[nodiscard]] RGTexture AddTo(RenderGraph &graph) const;

private:
    struct ViewUBO
    {
        Mat4 view_projection_matrix;
    };

    RGTextureDesc shadow_map_desc_;

    RHIResourceRef<RHIBuffer> view_buffer_;
};
} // namespace sparkle
