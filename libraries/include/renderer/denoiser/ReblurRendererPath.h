#pragma once

#include "core/math/Types.h"
#include "renderer/RenderConfig.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRayTracing.h"
#include "rhi/RHIRenderTarget.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIShader.h"

#include <memory>

namespace sparkle
{
class BindlessManager;
class CameraRenderProxy;
class ClearTexturePass;
class DirectionalLightRenderProxy;
class ReblurDenoiser;
class RHIContext;
class SkyRenderProxy;

struct ReblurFrameParameters
{
    uint32_t time_seed = 0;
    uint32_t total_sample_count = 0;
    uint32_t sample_per_pixel = 1;
    uint32_t dispatched_sample_count = 0;
    bool enable_nee = false;
    RenderConfig::DebugMode debug_mode = RenderConfig::DebugMode::Color;
};

class ReblurRendererPath
{
public:
    ReblurRendererPath(const RenderConfig &render_config, RHIContext *rhi, Vector2UInt image_size,
                       RHIImage *scene_texture, RHIImage *performance_sample_stats, RHITLAS *tlas);
    ~ReblurRendererPath();

    void BindBindlessResources(const BindlessManager &bindless_manager);
    void BindSkyLight(const SkyRenderProxy *sky_light);
    void BindTlas(RHITLAS *tlas, bool force_rebind = false);
    void ClearPathAccumulation();
    void UpdateFrameData(const CameraRenderProxy &camera, const SkyRenderProxy *sky_light,
                         const DirectionalLightRenderProxy *dir_light, const ReblurFrameParameters &parameters);
    void Render(const CameraRenderProxy &camera);
    void Reset();
    void ResetFinalHistory();

private:
    void CreateResources();
    void StabilizeFinalHistory();
    [[nodiscard]] bool ShouldStabilizeFinalHistory() const;

    const RenderConfig &render_config_;
    RHIContext *rhi_;
    Vector2UInt image_size_;
    RHIImage *scene_texture_ = nullptr;
    RHIImage *performance_sample_stats_ = nullptr;
    RHITLAS *tlas_ = nullptr;
    uint32_t dispatched_sample_count_ = 0;

    std::unique_ptr<ReblurDenoiser> reblur_;

    RHIResourceRef<RHIImage> pt_accumulation_;
    RHIResourceRef<RHIRenderTarget> pt_accumulation_rt_;
    std::unique_ptr<ClearTexturePass> pt_clear_pass_;

    RHIResourceRef<RHIImage> diffuse_signal_;
    RHIResourceRef<RHIImage> specular_signal_;
    RHIResourceRef<RHIImage> normal_roughness_;
    RHIResourceRef<RHIImage> view_z_;
    RHIResourceRef<RHIImage> motion_vectors_;
    RHIResourceRef<RHIImage> albedo_metallic_;

    RHIResourceRef<RHIShader> split_pt_shader_;
    RHIResourceRef<RHIPipelineState> split_pt_pipeline_;
    RHIResourceRef<RHIBuffer> split_pt_uniform_buffer_;

    RHIResourceRef<RHIShader> composite_shader_;
    RHIResourceRef<RHIPipelineState> composite_pipeline_;
    RHIResourceRef<RHIBuffer> composite_uniform_buffer_;
    RHIResourceRef<RHIComputePass> compute_pass_;

    RHIResourceRef<RHIImage> final_history_[2];
    RHIResourceRef<RHIImage> prev_final_view_z_;
    RHIResourceRef<RHIImage> prev_final_normal_roughness_;
    RHIResourceRef<RHIShader> final_history_shader_;
    RHIResourceRef<RHIPipelineState> final_history_pipeline_;
    RHIResourceRef<RHIBuffer> final_history_uniform_buffer_;
    uint32_t final_history_ping_pong_ = 0;
    bool final_history_valid_ = false;
};
} // namespace sparkle
