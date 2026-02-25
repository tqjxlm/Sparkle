#pragma once

#include "core/math/Types.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIResource.h"

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

    RHIContext *rhi_;
    uint32_t width_;
    uint32_t height_;
    uint32_t internal_frame_index_ = 0;
    bool history_valid_ = false;

    RHIResourceRef<RHIImage> denoised_diffuse_;
    RHIResourceRef<RHIImage> denoised_specular_;
};
} // namespace sparkle
