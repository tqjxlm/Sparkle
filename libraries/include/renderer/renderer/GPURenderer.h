#pragma once

#include "renderer/renderer/Renderer.h"

#include "rhi/RHIComputePass.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRayTracing.h"
#include "rhi/RHIShader.h"

namespace sparkle
{
class SkyRenderProxy;

class GPURenderer : public Renderer
{
public:
    GPURenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy);

    [[nodiscard]] RenderConfig::Pipeline GetRenderMode() const override
    {
        return RenderConfig::Pipeline::gpu;
    }

    void Render() override;

    void InitRenderResources() override;

    ~GPURenderer() override;

private:
    void Update() override;

    void InitSceneRenderResources();

    void BindBindlessResources();

    void MeasurePerformance();

    RHIResourceRef<RHIShader> compute_shader_;
    RHIResourceRef<RHIComputePass> compute_pass_;

    RHIResourceRef<RHIBuffer> uniform_buffer_;
    RHIResourceRef<RHITLAS> tlas_;

    RHIResourceRef<RHIImage> scene_texture_;
    RHIResourceRef<RHIRenderTarget> scene_rt_;
    std::unique_ptr<class ScreenQuadPass> screen_quad_pass_;

    RHIResourceRef<RHIImage> tone_mapping_output_;
    RHIResourceRef<RHIRenderTarget> tone_mapping_rt_;
    std::unique_ptr<class ToneMappingPass> tone_mapping_pass_;

    std::unique_ptr<class ClearTexturePass> clear_pass_;
    std::unique_ptr<class UiPass> ui_pass_;

    RHIResourceRef<RHIPipelineState> pipeline_state_;

    SkyRenderProxy *bound_sky_proxy_ = nullptr;

    struct ComputePerformanceRecord
    {
        uint32_t spp = 0;
    };

    std::vector<ComputePerformanceRecord> performance_history_;
    float running_time_per_spp_ = 0.f;

    uint32_t last_second_total_spp_ = 0;
};
} // namespace sparkle
