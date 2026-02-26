#pragma once

#include "core/math/Types.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIShader.h"

namespace sparkle
{
class RHIContext;

class ReblurDenoiser
{
public:
    static constexpr uint32_t ReblurTileSize = 16u;

    enum class HitDistanceReconstructionMode : uint32_t
    {
        Off = 0,
        Area3x3 = 1,
        Area5x5 = 2,
    };

    struct Settings
    {
        HitDistanceReconstructionMode hit_distance_reconstruction_mode = HitDistanceReconstructionMode::Area3x3;
        float prepass_diffuse_radius = 2.0f;
        float prepass_specular_radius = 2.0f;
        float prepass_spec_tracking_radius = 2.0f;
    };

    struct FrontEndInputs
    {
        RHIResourceRef<RHIImage> noisy_input;
        RHIResourceRef<RHIImage> normal_roughness;
        RHIResourceRef<RHIImage> view_z;
        RHIResourceRef<RHIImage> motion_vectors;
        RHIResourceRef<RHIImage> diff_radiance_hitdist;
        RHIResourceRef<RHIImage> spec_radiance_hitdist;
    };

    explicit ReblurDenoiser(RHIContext *rhi_context);

    void Initialize(const Vector2UInt &image_size);

    void Resize(const Vector2UInt &image_size);

    void SetSettings(const Settings &settings);

    void Dispatch(const FrontEndInputs &inputs, const RHIResourceRef<RHIImage> &denoised_output);

private:
    void CreateTileMaskTexture();
    void CreateHitDistanceReconstructionTextures();
    void CreatePrePassTextures();

    RHIContext *rhi_ = nullptr;
    Vector2UInt image_size_{};
    Vector2UInt tile_resolution_{};
    float denoising_range_ = 1000000.0f;
    Settings settings_{};

    RHIResourceRef<RHIShader> classify_tiles_shader_;
    RHIResourceRef<RHIShader> hit_distance_reconstruction_shader_;
    RHIResourceRef<RHIShader> prepass_shader_;
    RHIResourceRef<RHIShader> passthrough_shader_;
    RHIResourceRef<RHIPipelineState> classify_tiles_pipeline_state_;
    RHIResourceRef<RHIPipelineState> hit_distance_reconstruction_pipeline_state_;
    RHIResourceRef<RHIPipelineState> prepass_pipeline_state_;
    RHIResourceRef<RHIPipelineState> pipeline_state_;
    RHIResourceRef<RHIComputePass> classify_tiles_compute_pass_;
    RHIResourceRef<RHIComputePass> hit_distance_reconstruction_compute_pass_;
    RHIResourceRef<RHIComputePass> prepass_compute_pass_;
    RHIResourceRef<RHIComputePass> compute_pass_;
    RHIResourceRef<RHIBuffer> classify_tiles_uniform_buffer_;
    RHIResourceRef<RHIBuffer> hit_distance_reconstruction_uniform_buffer_;
    RHIResourceRef<RHIBuffer> prepass_uniform_buffer_;
    RHIResourceRef<RHIBuffer> uniform_buffer_;
    RHIResourceRef<RHIImage> tile_mask_texture_;
    RHIResourceRef<RHIImage> reconstructed_diff_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> reconstructed_spec_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> prepass_diff_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> prepass_spec_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> spec_hit_distance_for_tracking_texture_;
};
} // namespace sparkle
