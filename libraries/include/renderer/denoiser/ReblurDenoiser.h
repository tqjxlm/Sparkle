#pragma once

#include "core/math/Types.h"
#include "renderer/RenderConfig.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIShader.h"

namespace sparkle
{
class RHIContext;

struct ReblurSettings
{
    float max_blur_radius = 30.f;
    float min_blur_radius = 1.f;
    float diffuse_prepass_blur_radius = 30.f;
    float specular_prepass_blur_radius = 50.f;
    uint32_t max_accumulated_frame_num = 63;
    uint32_t max_stabilized_frame_num = 63;
    uint32_t history_fix_frame_num = 3;
    float history_fix_stride = 14.f;
    float disocclusion_threshold = 0.01f;
    float lobe_angle_fraction = 0.15f;
    float roughness_fraction = 0.15f;
    float plane_dist_sensitivity = 0.02f;
    float min_hit_dist_weight = 0.1f;
    float hit_dist_params[4] = {3.f, 0.1f, 20.f, -25.f};
    float stabilization_strength = 1.0f;
    float antilag_sigma_scale = 2.f;
    float antilag_sensitivity = 3.f;
    float fast_history_sigma_scale = 2.f;
    bool enable_anti_firefly = true;
};

struct ReblurInputBuffers
{
    RHIImage *diffuse_radiance_hit_dist = nullptr;
    RHIImage *specular_radiance_hit_dist = nullptr;
    RHIImage *normal_roughness = nullptr;
    RHIImage *view_z = nullptr;
    RHIImage *motion_vectors = nullptr;
    RHIImage *albedo_metallic = nullptr;
};

struct ReblurMatrices
{
    Mat4 view_to_clip = Mat4::Identity();
    Mat4 view_to_world = Mat4::Identity();
    Mat4 world_to_clip_prev = Mat4::Identity();
    Mat4 world_to_view_prev = Mat4::Identity();
    Mat4 world_prev_to_world = Mat4::Identity();
    Vector3 camera_delta = Vector3::Zero();
    float framerate_scale = 1.0f;
};

class ReblurDenoiser
{
public:
    ReblurDenoiser(RHIContext *rhi, uint32_t width, uint32_t height);
    ~ReblurDenoiser();

    void Denoise(const ReblurInputBuffers &inputs, const ReblurSettings &settings, const ReblurMatrices &matrices,
                 uint32_t frame_index,
                 RenderConfig::ReblurDebugPass debug_pass = RenderConfig::ReblurDebugPass::Full);

    [[nodiscard]] RHIImage *GetDenoisedDiffuse() const;
    [[nodiscard]] RHIImage *GetDenoisedSpecular() const;
    [[nodiscard]] RHIImage *GetInternalData() const;

    void Reset();

private:
    void CreateTextures();
    void CreatePipelines();
    void ClassifyTiles(const ReblurInputBuffers &inputs, const ReblurSettings &settings);
    void Blur(const ReblurInputBuffers &inputs, const ReblurSettings &settings, const ReblurMatrices &matrices,
              uint32_t pass_index, RHIImage *in_diff, RHIImage *in_spec, RHIImage *out_diff, RHIImage *out_spec,
              RHIImage *internal_data, bool has_temporal_data);
    void TemporalAccumulate(const ReblurInputBuffers &inputs, const ReblurSettings &settings,
                            uint32_t debug_output = 0, bool use_alt_pipeline = false);
    void HistoryFix(const ReblurInputBuffers &inputs, const ReblurSettings &settings, const ReblurMatrices &matrices);
    void TemporalStabilize(const ReblurInputBuffers &inputs, const ReblurSettings &settings,
                           const ReblurMatrices &matrices);
    void CopyToOutput(RHIImage *diff, RHIImage *spec);
    void CopyPreviousFrameData(const ReblurInputBuffers &inputs);
    void CopyHistoryData(RHIImage *diff, RHIImage *spec);
    void CopyStabilizedHistory(RHIImage *diff, RHIImage *spec);

    RHIContext *rhi_;
    uint32_t width_;
    uint32_t height_;
    uint32_t internal_frame_index_ = 0;
    bool history_valid_ = false;

    RHIResourceRef<RHIComputePass> compute_pass_;

    // Denoised output
    RHIResourceRef<RHIImage> denoised_diffuse_;
    RHIResourceRef<RHIImage> denoised_specular_;

    // Ping-pong temp textures for blur passes
    RHIResourceRef<RHIImage> diff_temp1_;
    RHIResourceRef<RHIImage> diff_temp2_;
    RHIResourceRef<RHIImage> spec_temp1_;
    RHIResourceRef<RHIImage> spec_temp2_;

    // History buffers (persistent across frames)
    RHIResourceRef<RHIImage> diff_history_;
    RHIResourceRef<RHIImage> spec_history_;

    // Internal data: packed accumSpeed (slow + fast)
    RHIResourceRef<RHIImage> internal_data_;
    RHIResourceRef<RHIImage> prev_internal_data_;

    // Previous-frame buffers
    RHIResourceRef<RHIImage> prev_view_z_;
    RHIResourceRef<RHIImage> prev_normal_roughness_;

    // Temporal accumulation pass
    RHIResourceRef<RHIShader> temporal_accum_shader_;
    RHIResourceRef<RHIPipelineState> temporal_accum_pipeline_;
    RHIResourceRef<RHIBuffer> temporal_accum_ub_;

    // Second TA pipeline for diagnostic two-pass mode.
    // Needed because dynamic UBO uploads (memcpy) overwrite the first pass's
    // UBO data before the GPU reads it. Using a separate pipeline+UBO avoids this.
    RHIResourceRef<RHIPipelineState> temporal_accum_pipeline_alt_;
    RHIResourceRef<RHIBuffer> temporal_accum_ub_alt_;

    // History fix pass
    RHIResourceRef<RHIShader> history_fix_shader_;
    RHIResourceRef<RHIPipelineState> history_fix_pipeline_;
    RHIResourceRef<RHIBuffer> history_fix_ub_;

    // ClassifyTiles pass
    RHIResourceRef<RHIImage> tiles_;
    RHIResourceRef<RHIShader> classify_tiles_shader_;
    RHIResourceRef<RHIPipelineState> classify_tiles_pipeline_;
    RHIResourceRef<RHIBuffer> classify_tiles_ub_;

    // Temporal stabilization pass
    RHIResourceRef<RHIShader> temporal_stab_shader_;
    RHIResourceRef<RHIPipelineState> temporal_stab_pipeline_;
    RHIResourceRef<RHIBuffer> temporal_stab_ub_;

    // Stabilized history (ping-pong)
    RHIResourceRef<RHIImage> diff_stabilized_[2];
    RHIResourceRef<RHIImage> spec_stabilized_[2];
    uint32_t stab_ping_pong_ = 0;

    // Blur passes (separate pipelines to avoid descriptor set conflicts within a frame)
    static constexpr uint32_t BlurPassCount = 3; // 0=PrePass, 1=Blur, 2=PostBlur
    RHIResourceRef<RHIShader> blur_shader_;
    RHIResourceRef<RHIPipelineState> blur_pipelines_[BlurPassCount];
    RHIResourceRef<RHIBuffer> blur_ubs_[BlurPassCount];
};
} // namespace sparkle
