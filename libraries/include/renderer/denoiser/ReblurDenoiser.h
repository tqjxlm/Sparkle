#pragma once

#include "core/math/Types.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIShader.h"
#include <array>

namespace sparkle
{
class RHIContext;

class ReblurDenoiser
{
public:
    static constexpr uint32_t ReblurTileSize = 16u;
    static constexpr uint32_t TemporalHistoryPingPongCount = 2u;

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
        float blur_min_radius = 1.0f;
        float blur_max_radius = 6.0f;
        uint32_t blur_history_max_frame_num = 32u;
    };

    struct FrontEndInputs
    {
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

    void ResetHistory();

    void Dispatch(const FrontEndInputs &inputs, const RHIResourceRef<RHIImage> &denoised_output);

private:
    void CreateTileMaskTexture();
    void CreateHitDistanceReconstructionTextures();
    void CreatePrePassTextures();
    void CreateTemporalTextures();
    void CreateBlurTextures();

    RHIContext *rhi_ = nullptr;
    Vector2UInt image_size_{};
    Vector2UInt tile_resolution_{};
    float denoising_range_ = 1000000.0f;
    Settings settings_{};

    RHIResourceRef<RHIShader> classify_tiles_shader_;
    RHIResourceRef<RHIShader> hit_distance_reconstruction_shader_;
    RHIResourceRef<RHIShader> prepass_shader_;
    RHIResourceRef<RHIShader> temporal_accumulation_shader_;
    RHIResourceRef<RHIShader> blur_shader_;
    RHIResourceRef<RHIShader> post_blur_shader_;
    RHIResourceRef<RHIPipelineState> classify_tiles_pipeline_state_;
    RHIResourceRef<RHIPipelineState> hit_distance_reconstruction_pipeline_state_;
    RHIResourceRef<RHIPipelineState> prepass_pipeline_state_;
    RHIResourceRef<RHIPipelineState> temporal_accumulation_pipeline_state_;
    RHIResourceRef<RHIPipelineState> blur_pipeline_state_;
    RHIResourceRef<RHIPipelineState> post_blur_pipeline_state_;
    RHIResourceRef<RHIComputePass> classify_tiles_compute_pass_;
    RHIResourceRef<RHIComputePass> hit_distance_reconstruction_compute_pass_;
    RHIResourceRef<RHIComputePass> prepass_compute_pass_;
    RHIResourceRef<RHIComputePass> temporal_accumulation_compute_pass_;
    RHIResourceRef<RHIComputePass> blur_compute_pass_;
    RHIResourceRef<RHIComputePass> post_blur_compute_pass_;
    RHIResourceRef<RHIBuffer> classify_tiles_uniform_buffer_;
    RHIResourceRef<RHIBuffer> hit_distance_reconstruction_uniform_buffer_;
    RHIResourceRef<RHIBuffer> prepass_uniform_buffer_;
    RHIResourceRef<RHIBuffer> temporal_accumulation_uniform_buffer_;
    RHIResourceRef<RHIBuffer> blur_uniform_buffer_;
    RHIResourceRef<RHIBuffer> post_blur_uniform_buffer_;
    RHIResourceRef<RHIImage> tile_mask_texture_;
    RHIResourceRef<RHIImage> reconstructed_diff_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> reconstructed_spec_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> prepass_diff_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> prepass_spec_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> spec_hit_distance_for_tracking_texture_;
    RHIResourceRef<RHIImage> data1_texture_;
    RHIResourceRef<RHIImage> data2_texture_;
    RHIResourceRef<RHIImage> temporal_diff_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> temporal_spec_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> prev_normal_roughness_texture_;
    std::array<RHIResourceRef<RHIImage>, TemporalHistoryPingPongCount> diff_history_textures_;
    std::array<RHIResourceRef<RHIImage>, TemporalHistoryPingPongCount> spec_history_textures_;
    std::array<RHIResourceRef<RHIImage>, TemporalHistoryPingPongCount> diff_fast_history_textures_;
    std::array<RHIResourceRef<RHIImage>, TemporalHistoryPingPongCount> spec_fast_history_textures_;
    std::array<RHIResourceRef<RHIImage>, TemporalHistoryPingPongCount> spec_hit_distance_tracking_history_textures_;
    std::array<RHIResourceRef<RHIImage>, TemporalHistoryPingPongCount> internal_data_textures_;
    RHIResourceRef<RHIImage> blur_diff_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> blur_spec_radiance_hitdist_texture_;
    RHIResourceRef<RHIImage> prev_view_z_texture_;
    uint32_t temporal_history_read_index_ = 0u;
    bool temporal_history_valid_ = false;
};
} // namespace sparkle
