#pragma once

#include "renderer/pass/PipelinePass.h"

#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRenderPass.h"
#include "rhi/RHIRenderTarget.h"

namespace sparkle
{
class SkyRenderProxy;

class SkyBoxPass : public PipelinePass
{
public:
    explicit SkyBoxPass(RHIContext *rhi, const SkyRenderProxy *sky_proxy, const RHIResourceRef<RHIImage> &color_buffer,
                        const RHIResourceRef<RHIImage> &depth_buffer);

    void Render() override;

    void InitRenderResources(const RenderConfig &config) override;

    void UpdateFrameData(const RenderConfig & /*config*/, SceneRenderProxy * /*scene*/) override;

    void OverrideSkyMap(const RHIResourceRef<RHIImage> &sky_map);

private:
    void SetupVertexShader();
    void SetupPixelShader();
    void SetupVertices();
    void BindShaderResources();

    const SkyRenderProxy *sky_proxy_;

    RHIResourceRef<RHIRenderPass> render_pass_;
    RHIResourceRef<RHIRenderTarget> render_target_;

    struct ScreenVertex
    {
        Vector3 position;
        Vector2 uv;
    };

    RHIResourceRef<RHIBuffer> vertex_buffer_;
    RHIResourceRef<RHIBuffer> index_buffer_;

    RHIResourceRef<RHIPipelineState> pipeline_state_;

    RHIResourceRef<RHIBuffer> vs_ub_;
    RHIResourceRef<RHIBuffer> ps_ub_;

    RHIResourceRef<RHIImage> sky_map_to_render_;
    RHIResourceRef<RHIImage> color_buffer_;
    RHIResourceRef<RHIImage> depth_buffer_;

    DrawArgs draw_args_;
};
} // namespace sparkle
