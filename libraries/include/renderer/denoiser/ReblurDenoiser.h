#pragma once

#include "core/math/Types.h"
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
    uint32_t max_accumulated_frame_num = 30;
    uint32_t max_fast_accumulated_frame_num = 6;
    uint32_t max_stabilized_frame_num = 63;
    uint32_t history_fix_frame_num = 3;
    float history_fix_stride = 14.f;
    float disocclusion_threshold = 0.01f;
    float lobe_angle_fraction = 0.15f;
    float roughness_fraction = 0.15f;
    float plane_dist_sensitivity = 0.02f;
    float min_hit_dist_weight = 0.1f;
    float hit_dist_params[4] = {3.f, 0.1f, 20.f, -25.f};
    float stabilization_strength = 1.f;
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
};

class ReblurDenoiser
{
public:
    ReblurDenoiser(RHIContext *rhi, uint32_t width, uint32_t height);
    ~ReblurDenoiser();

    void Denoise(const ReblurInputBuffers &inputs, const ReblurSettings &settings, const ReblurMatrices &matrices,
                 uint32_t frame_index);

    [[nodiscard]] RHIImage *GetDenoisedDiffuse() const;
    [[nodiscard]] RHIImage *GetDenoisedSpecular() const;

    void Reset();

private:
    void CreateTextures();
    void CreatePipelines();
    void ClassifyTiles(const ReblurInputBuffers &inputs, const ReblurSettings &settings);
    void Blur(const ReblurInputBuffers &inputs, const ReblurSettings &settings, uint32_t pass_index,
              RHIImage *in_diff, RHIImage *in_spec, RHIImage *out_diff, RHIImage *out_spec);

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

    // ClassifyTiles pass
    RHIResourceRef<RHIImage> tiles_;
    RHIResourceRef<RHIShader> classify_tiles_shader_;
    RHIResourceRef<RHIPipelineState> classify_tiles_pipeline_;
    RHIResourceRef<RHIBuffer> classify_tiles_ub_;

    // Blur pass (shared by PrePass, Blur, PostBlur)
    RHIResourceRef<RHIShader> blur_shader_;
    RHIResourceRef<RHIPipelineState> blur_pipeline_;
    RHIResourceRef<RHIBuffer> blur_ub_;
};
} // namespace sparkle
