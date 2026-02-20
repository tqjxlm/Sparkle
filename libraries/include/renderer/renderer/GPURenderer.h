#pragma once

#include "renderer/renderer/Renderer.h"

#include "core/Timer.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRayTracing.h"
#include "rhi/RHIShader.h"

#include <array>

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

    [[nodiscard]] bool IsReadyForAutoScreenshot() const override;

    ~GPURenderer() override;

private:
    void Update() override;

    void InitSceneRenderResources();

    void InitASVGFRenderResources();

    void BindBindlessResources();

    void RunASVGFPasses();

    void ResetASVGFHistoryResources();

    [[nodiscard]] bool UseASVGFDebugDisplay() const;

    [[nodiscard]] uint32_t GetRayTraceDebugMode() const;

    void MeasurePerformance();

    RHIResourceRef<RHIShader> compute_shader_;
    RHIResourceRef<RHIComputePass> compute_pass_;

    RHIResourceRef<RHIBuffer> uniform_buffer_;
    RHIResourceRef<RHITLAS> tlas_;

    RHIResourceRef<RHIImage> scene_texture_;
    RHIResourceRef<RHIRenderTarget> scene_rt_;
    std::unique_ptr<class ScreenQuadPass> screen_quad_pass_;

    RHIResourceRef<RHIImage> asvgf_noisy_texture_;
    RHIResourceRef<RHIImage> asvgf_feature_normal_roughness_texture_;
    RHIResourceRef<RHIImage> asvgf_feature_albedo_metallic_texture_;
    RHIResourceRef<RHIImage> asvgf_feature_depth_texture_;
    RHIResourceRef<RHIImage> asvgf_feature_primitive_id_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> asvgf_history_feature_normal_roughness_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> asvgf_history_feature_depth_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> asvgf_history_feature_primitive_id_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> asvgf_history_color_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> asvgf_history_moments_texture_;
    RHIResourceRef<RHIImage> asvgf_variance_texture_;
    RHIResourceRef<RHIImage> asvgf_reprojection_mask_texture_;
    RHIResourceRef<RHIImage> asvgf_reprojection_debug_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> asvgf_atrous_ping_pong_texture_;
    RHIResourceRef<RHIImage> asvgf_debug_texture_;
    RHIResourceRef<RHIImage> asvgf_fallback_noisy_texture_;
    RHIResourceRef<RHIImage> asvgf_fallback_normal_roughness_texture_;
    RHIResourceRef<RHIImage> asvgf_fallback_albedo_metallic_texture_;
    RHIResourceRef<RHIImage> asvgf_fallback_depth_texture_;
    RHIResourceRef<RHIImage> asvgf_fallback_primitive_id_texture_;

    RHIResourceRef<RHIImage> tone_mapping_output_;
    RHIResourceRef<RHIRenderTarget> tone_mapping_rt_;
    std::unique_ptr<class ToneMappingPass> tone_mapping_pass_;
    std::unique_ptr<class ToneMappingPass> asvgf_debug_tone_mapping_pass_;

    std::unique_ptr<class ClearTexturePass> clear_pass_;
    std::unique_ptr<class UiPass> ui_pass_;

    RHIResourceRef<RHIPipelineState> pipeline_state_;
    RHIResourceRef<RHIPipelineState> asvgf_reprojection_pipeline_state_;
    RHIResourceRef<RHIPipelineState> asvgf_variance_pipeline_state_;
    std::array<RHIResourceRef<RHIPipelineState>, 8> asvgf_atrous_pipeline_states_;
    RHIResourceRef<RHIPipelineState> asvgf_debug_pipeline_state_;
    RHIResourceRef<RHIShader> asvgf_reprojection_shader_;
    RHIResourceRef<RHIShader> asvgf_variance_shader_;
    RHIResourceRef<RHIShader> asvgf_atrous_shader_;
    RHIResourceRef<RHIShader> asvgf_debug_shader_;

    SkyRenderProxy *bound_sky_proxy_ = nullptr;

    RHIResourceRef<RHIBuffer> asvgf_reprojection_uniform_buffer_;
    RHIResourceRef<RHIBuffer> asvgf_variance_uniform_buffer_;
    RHIResourceRef<RHIBuffer> asvgf_atrous_uniform_buffer_;
    RHIResourceRef<RHIBuffer> asvgf_debug_uniform_buffer_;
    RHIResourceRef<RHIComputePass> asvgf_reprojection_compute_pass_;
    RHIResourceRef<RHIComputePass> asvgf_variance_compute_pass_;
    RHIResourceRef<RHIComputePass> asvgf_atrous_compute_pass_;
    RHIResourceRef<RHIComputePass> asvgf_debug_compute_pass_;

    struct ComputePerformanceRecord
    {
        uint32_t spp = 0;
    };

    std::vector<ComputePerformanceRecord> performance_history_;
    float running_time_per_spp_ = 0.f;

    uint32_t last_second_total_spp_ = 0;
    uint32_t dispatched_sample_count_ = 0;
    uint32_t asvgf_current_frame_spp_ = 1;

    uint32_t asvgf_history_index_ = 0;
    bool asvgf_history_clear_pending_ = true;
    Vector3 asvgf_previous_camera_position_ = Vector3::Zero();
    Vector3 asvgf_previous_lower_left_ = Vector3::Zero();
    Vector3 asvgf_previous_max_u_ = Vector3::Zero();
    Vector3 asvgf_previous_max_v_ = Vector3::Zero();
    bool asvgf_previous_camera_valid_ = false;

    RenderConfig::ASVGFDebugView last_asvgf_debug_view_ = RenderConfig::ASVGFDebugView::none;
    RenderConfig::ASVGFTestStage last_asvgf_test_stage_ = RenderConfig::ASVGFTestStage::off;
    bool last_asvgf_enabled_ = false;

    TimerCaller spp_logger_;
};
} // namespace sparkle
