#pragma once

#include "renderer/renderer/Renderer.h"

#include "core/Timer.h"
#include "renderer/denoiser/DenoiserConfig.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRayTracing.h"
#include "rhi/RHIShader.h"

namespace sparkle
{
class SkyRenderProxy;
class RHIDenoiser;
class PathTracingDenoiserInputs;

class GPURenderer : public Renderer
{
public:
    GPURenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy);

    [[nodiscard]] RenderConfig::Pipeline GetRenderMode() const override
    {
        return RenderConfig::Pipeline::Gpu;
    }

    void Render() override;

    void InitRenderResources() override;

    [[nodiscard]] bool IsReadyForAutoScreenshot() const override;

    ~GPURenderer() override;

private:
    void Update() override;

    void InitSceneRenderResources();

    void BindDenoiserInputs();

    [[nodiscard]] RHIDenoiser *GetOrCreateDenoiser(DenoiserProvider provider);

    [[nodiscard]] RHIDenoiser *SelectDenoiser(DenoiserProvider requested, DenoiserProvider &effective);

    void BindBindlessResources();

    void MeasurePerformance();

    [[nodiscard]] bool AccumulationPaused() const
    {
        return render_config_.manual_accumulation &&
               !(render_config_.accumulate_key_held || render_config_.accumulate_button_held);
    }

    RHIResourceRef<RHIShader> compute_shader_;
    RHIResourceRef<RHIComputePass> compute_pass_;

    RHIResourceRef<RHIBuffer> uniform_buffer_;
    RHIResourceRef<RHITLAS> tlas_;

    RHIResourceRef<RHIImage> scene_texture_;
    RHIResourceRef<RHIRenderTarget> scene_rt_;
    std::unique_ptr<PathTracingDenoiserInputs> denoiser_inputs_;
    std::unique_ptr<RHIDenoiser> nrd_denoiser_;
    std::unique_ptr<RHIDenoiser> metalfx_denoiser_;
    std::unique_ptr<class ScreenQuadPass> screen_quad_pass_;

    RHIResourceRef<RHIImage> tone_mapping_output_;
    RHIResourceRef<RHIRenderTarget> tone_mapping_rt_;
    std::unique_ptr<class ToneMappingPass> tone_mapping_pass_;

    std::unique_ptr<class ClearTexturePass> clear_pass_;
    std::unique_ptr<class UiPass> ui_pass_;

    RHIResourceRef<RHIPipelineState> pipeline_state_;

    SkyRenderProxy *bound_sky_proxy_ = nullptr;

    RHIDenoiser *frame_denoiser_ = nullptr;
    DenoiserProvider frame_provider_ = DenoiserProvider::Off;
    DenoiserProvider requested_provider_ = DenoiserProvider::Off;
    bool nrd_failed_ = false;
    bool metalfx_failed_ = false;
    bool gbuffer_write_this_frame_ = false;
    bool denoiser_reset_this_frame_ = false;
    bool final_frame_this_frame_ = false;
    bool scene_ready_last_ = false;

    struct ComputePerformanceRecord
    {
        uint32_t spp = 0;
    };

    std::vector<ComputePerformanceRecord> performance_history_;
    float running_time_per_spp_ = 0.f;

    uint32_t last_second_total_spp_ = 0;
    uint32_t seed_counter_ = 0;

    TimerCaller spp_logger_;
};
} // namespace sparkle
