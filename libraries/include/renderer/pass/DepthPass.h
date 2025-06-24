#pragma once

#include "renderer/pass/MeshPass.h"

#include "core/math/Types.h"
#include "rhi/RHIBuffer.h"
#include "rhi/RHIRenderTarget.h"

namespace sparkle
{
class DepthPass : public MeshPass
{
public:
    DepthPass(RHIContext *ctx, SceneRenderProxy *scene_proxy, unsigned width, unsigned height);

    void Render() override;

    void InitRenderResources(const RenderConfig &config) override;

    void HandleNewPrimitive(uint32_t primitive_id) override;

    void HandleUpdatedPrimitive([[maybe_unused]] uint32_t primitive_id) override
    {
    }

    [[nodiscard]] RHIResourceRef<RHIRenderTarget> GetOutput() const
    {
        return depth_target_;
    }

    void SetProjectionMatrix(const Mat4 &matrix);

private:
    struct ViewUBO
    {
        Mat4 view_projection_matrix;
    };

    RHIResourceRef<RHIImage> depth_texture_;

    RHIResourceRef<RHIRenderTarget> depth_target_;
    RHIResourceRef<RHIBuffer> view_buffer_;
    RHIResourceRef<RHIRenderPass> pass_;

    unsigned width_;
    unsigned height_;
};
} // namespace sparkle
