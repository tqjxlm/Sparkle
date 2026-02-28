#pragma once

#include "renderer/renderer/Renderer.h"

#include "core/Timer.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRayTracing.h"
#include "rhi/RHIShader.h"

#include <memory>

namespace sparkle
{
class SkyRenderProxy;
class ReblurDenoiser;

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

    [[nodiscard]] bool IsReadyForAutoScreenshot() const override;

    ~GPURenderer() override;

private:
    void Update() override;

    void InitSceneRenderResources();

    void BindBindlessResources();

    void MeasurePerformance();

    void InitReblurResources();

    void RenderReblurPath();

    void BindSplitBindlessResources();

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

    // REBLUR denoiser (null when disabled)
    std::unique_ptr<ReblurDenoiser> reblur_;

    // Separate PT accumulation buffer (not shared with composite output)
    RHIResourceRef<RHIImage> pt_accumulation_;
    RHIResourceRef<RHIRenderTarget> pt_accumulation_rt_;
    std::unique_ptr<class ClearTexturePass> pt_clear_pass_;

    // Auxiliary buffers for split path tracer
    RHIResourceRef<RHIImage> diffuse_signal_;
    RHIResourceRef<RHIImage> specular_signal_;
    RHIResourceRef<RHIImage> normal_roughness_;
    RHIResourceRef<RHIImage> view_z_;
    RHIResourceRef<RHIImage> motion_vectors_;
    RHIResourceRef<RHIImage> albedo_metallic_;

    // Split path tracer pipeline
    RHIResourceRef<RHIShader> split_pt_shader_;
    RHIResourceRef<RHIPipelineState> split_pt_pipeline_;
    RHIResourceRef<RHIBuffer> split_pt_uniform_buffer_;

    // Composite pipeline
    RHIResourceRef<RHIShader> composite_shader_;
    RHIResourceRef<RHIPipelineState> composite_pipeline_;
    RHIResourceRef<RHIBuffer> composite_uniform_buffer_;
    RHIResourceRef<RHIComputePass> reblur_compute_pass_;

    struct ComputePerformanceRecord
    {
        uint32_t spp = 0;
    };

    std::vector<ComputePerformanceRecord> performance_history_;
    float running_time_per_spp_ = 0.f;

    uint32_t last_second_total_spp_ = 0;
    uint32_t dispatched_sample_count_ = 0;

    TimerCaller spp_logger_;
};
} // namespace sparkle
