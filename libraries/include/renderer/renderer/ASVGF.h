#pragma once

#include "renderer/RenderConfig.h"

#include "rhi/RHIBuffer.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRenderTarget.h"
#include "rhi/RHIShader.h"

#include <array>
#include <memory>

namespace sparkle
{
class SceneRenderProxy;
class ToneMappingPass;

class ASVGF
{
public:
    struct RayTraceOutputTextures
    {
        RHIResourceRef<RHIImage> noisy_texture;
        RHIResourceRef<RHIImage> feature_normal_roughness_texture;
        RHIResourceRef<RHIImage> feature_albedo_metallic_texture;
        RHIResourceRef<RHIImage> feature_depth_texture;
        RHIResourceRef<RHIImage> feature_primitive_id_texture;

        [[nodiscard]] bool IsValid() const;
    };

    ASVGF(const RenderConfig &render_config, const Vector2UInt &image_size, RHIContext *rhi,
          SceneRenderProxy *scene_render_proxy, const RHIResourceRef<RHIImage> &scene_texture,
          const RHIResourceRef<RHIRenderTarget> &tone_mapping_rt);

    void InitFallbackRenderResources();

    void InitRenderResources(const RenderConfig &render_config);

    [[nodiscard]] bool HandleConfigStateChange(const RenderConfig &render_config);

    void MarkHistoryClearPending();

    [[nodiscard]] RayTraceOutputTextures GetRayTraceOutputTextures(const RenderConfig &render_config) const;

    void TransitionRayTraceOutputTexturesForWrite(const RenderConfig &render_config) const;

    void RunPasses(const RenderConfig &render_config, uint32_t current_frame_spp);

    void UpdateFrameData(const RenderConfig &render_config, SceneRenderProxy *scene_render_proxy);

    [[nodiscard]] bool UseDebugDisplay(const RenderConfig &render_config) const;

    [[nodiscard]] ToneMappingPass *GetDebugToneMappingPass() const;

    [[nodiscard]] bool IsInitialized() const;

private:
    [[nodiscard]] bool HasPipelineResources() const;

    [[nodiscard]] bool HasPassResources() const;

    [[nodiscard]] bool HasDenoiserTextures() const;

    void ResetHistoryResources();

    Vector2UInt image_size_ = Vector2UInt::Zero();
    RHIContext *rhi_ = nullptr;
    SceneRenderProxy *scene_render_proxy_ = nullptr;
    RHIResourceRef<RHIImage> scene_texture_;
    RHIResourceRef<RHIRenderTarget> tone_mapping_rt_;

    RHIResourceRef<RHIImage> noisy_texture_;
    RHIResourceRef<RHIImage> feature_normal_roughness_texture_;
    RHIResourceRef<RHIImage> feature_albedo_metallic_texture_;
    RHIResourceRef<RHIImage> feature_depth_texture_;
    RHIResourceRef<RHIImage> feature_primitive_id_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> history_feature_normal_roughness_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> history_feature_depth_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> history_feature_primitive_id_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> history_color_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> history_moments_texture_;
    RHIResourceRef<RHIImage> variance_texture_;
    RHIResourceRef<RHIImage> reprojection_mask_texture_;
    RHIResourceRef<RHIImage> reprojection_debug_texture_;
    std::array<RHIResourceRef<RHIImage>, 2> atrous_ping_pong_texture_;
    RHIResourceRef<RHIImage> debug_texture_;

    RayTraceOutputTextures fallback_ray_trace_output_textures_;

    std::unique_ptr<ToneMappingPass> debug_tone_mapping_pass_;

    RHIResourceRef<RHIPipelineState> reprojection_pipeline_state_;
    RHIResourceRef<RHIPipelineState> variance_pipeline_state_;
    std::array<RHIResourceRef<RHIPipelineState>, 8> atrous_pipeline_states_;
    RHIResourceRef<RHIPipelineState> debug_pipeline_state_;
    RHIResourceRef<RHIShader> reprojection_shader_;
    RHIResourceRef<RHIShader> variance_shader_;
    RHIResourceRef<RHIShader> atrous_shader_;
    RHIResourceRef<RHIShader> debug_shader_;

    RHIResourceRef<RHIBuffer> reprojection_uniform_buffer_;
    RHIResourceRef<RHIBuffer> variance_uniform_buffer_;
    RHIResourceRef<RHIBuffer> atrous_uniform_buffer_;
    RHIResourceRef<RHIBuffer> debug_uniform_buffer_;
    RHIResourceRef<RHIComputePass> reprojection_compute_pass_;
    RHIResourceRef<RHIComputePass> variance_compute_pass_;
    RHIResourceRef<RHIComputePass> atrous_compute_pass_;
    RHIResourceRef<RHIComputePass> debug_compute_pass_;

    uint32_t history_index_ = 0;
    bool history_clear_pending_ = true;
    Vector3 previous_camera_position_ = Vector3::Zero();
    Vector3 previous_lower_left_ = Vector3::Zero();
    Vector3 previous_max_u_ = Vector3::Zero();
    Vector3 previous_max_v_ = Vector3::Zero();
    bool previous_camera_valid_ = false;

    RenderConfig::ASVGFDebugView last_debug_view_ = RenderConfig::ASVGFDebugView::none;
    RenderConfig::ASVGFTestStage last_test_stage_ = RenderConfig::ASVGFTestStage::off;
    bool last_enabled_ = false;
};
} // namespace sparkle
