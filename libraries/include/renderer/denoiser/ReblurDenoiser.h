#pragma once

#include "core/math/Types.h"
#include "renderer/RenderConfig.h"
#include "renderer/denoiser/ReblurSignalGenerator.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRayTracing.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIShader.h"

#include <memory>

namespace sparkle
{
class BindlessManager;
class CameraRenderProxy;
class DirectionalLightRenderProxy;
class ReblurDenoisingPipeline;
class ReblurSignalGenerator;
class RHIContext;
class SkyRenderProxy;

class ReblurDenoiser
{
public:
    ReblurDenoiser(const RenderConfig &render_config, RHIContext *rhi, Vector2UInt image_size, RHIImage *scene_texture,
                   RHIImage *performance_sample_stats, RHITLAS *tlas);
    ~ReblurDenoiser();

    void BindBindlessResources(const BindlessManager &bindless_manager);
    void BindSkyLight(const SkyRenderProxy *sky_light);
    void BindTlas(RHITLAS *tlas, bool force_rebind = false);
    void ClearPathTracingAccumulation();
    void PrepareForPathTracing();
    void UpdateFrameData(const CameraRenderProxy &camera, const SkyRenderProxy *sky_light,
                         const DirectionalLightRenderProxy *dir_light, const ReblurPathTracingParameters &parameters);
    [[nodiscard]] const RHIResourceRef<RHIPipelineState> &GetPathTracingPipeline() const;
    void ResolveSceneTexture(const CameraRenderProxy &camera);
    void Reset();
    void ResetFinalHistory();

private:
    void CreateResources(RHIImage *performance_sample_stats, RHITLAS *tlas);
    void StabilizeFinalHistory();
    [[nodiscard]] bool ShouldStabilizeFinalHistory() const;

    const RenderConfig &render_config_;
    RHIContext *rhi_;
    Vector2UInt image_size_;
    RHIImage *scene_texture_ = nullptr;
    uint32_t dispatched_sample_count_ = 0;

    std::unique_ptr<ReblurSignalGenerator> signal_generator_;
    std::unique_ptr<ReblurDenoisingPipeline> denoising_pipeline_;

    RHIResourceRef<RHIShader> composite_shader_;
    RHIResourceRef<RHIPipelineState> composite_pipeline_;
    RHIResourceRef<RHIBuffer> composite_uniform_buffer_;
    RHIResourceRef<RHIComputePass> resolve_pass_;

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
