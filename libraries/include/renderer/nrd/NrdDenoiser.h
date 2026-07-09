#pragma once

#include "renderer/nrd/NrdConfig.h"
#include "renderer/pass/PipelinePass.h"

#include "core/math/Types.h"
#include "rhi/RHIBuffer.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHINrdBackend.h"
#include "rhi/RHIPIpelineState.h"

#include <memory>
#include <vector>

namespace nrd
{
struct Instance;
}

namespace sparkle
{
// NVIDIA NRD (ReBLUR_DIFFUSE_SPECULAR) denoiser for the gpu path tracer. Owns the GPU-agnostic
// nrd::Instance and hands SPIR-V + per-frame dispatches to an RHINrdBackend (Metal today). Also owns
// the G-buffer targets the path tracer writes when the denoiser is on: nrd_pack encodes them into
// NRD's input textures, the backend runs ReBLUR, and nrd_resolve re-modulates the denoised signal
// into output_ for tone mapping.
class NrdDenoiser : public PipelinePass
{
public:
    NrdDenoiser(RHIContext *ctx, RHIResourceRef<RHIImage> input);

    ~NrdDenoiser() override;

    void InitRenderResources(const RenderConfig &config) override;

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

    void Render() override;

    // Allocate the heavy resources (NRD instance/pipelines/pool + textures + passes) if not already.
    // Idempotent; called on launch when enabled, and on the first runtime enable.
    void EnsureEnabledResources();

    void RequestReset()
    {
        reset_history_ = true;
    }

    // Copy the cross-thread NrdConfig into the per-frame snapshot all denoiser logic reads. Call once
    // at the top of the renderer's Update so a control-panel write cannot tear a frame.
    void SampleConfig();

    [[nodiscard]] bool IsActive() const
    {
        return config_.enabled && !enabled_resources_failed_;
    }

    // false once the accumulator handoff completed (updated per frame in UpdateFrameData): the path
    // tracer may skip the G-buffer emission entirely
    [[nodiscard]] bool NeedsGBuffer() const
    {
        return needs_gbuffer_;
    }

    [[nodiscard]] RHIResourceRef<RHIImage> GetOutput() const
    {
        return output_;
    }

    // Transition the G-buffer targets to a writable layout before the path-tracer dispatch.
    void BeginGBufferWrite();

    // G-buffer targets the path tracer writes into. 1x1 dummies until the denoiser is first enabled
    // (the path-tracer pipeline always declares the bindings); the caller must re-bind them after
    // EnsureEnabledResources swaps in the real targets.
    [[nodiscard]] RHIResourceRef<RHIImage> GetRadiance() const
    {
        return g_radiance_;
    }

    [[nodiscard]] RHIResourceRef<RHIImage> GetNormalDepth() const
    {
        return g_normal_depth_;
    }

    [[nodiscard]] RHIResourceRef<RHIImage> GetAlbedoObj() const
    {
        return g_albedo_obj_;
    }

    [[nodiscard]] RHIResourceRef<RHIImage> GetMotion() const
    {
        return g_motion_;
    }

    [[nodiscard]] RHIResourceRef<RHIImage> GetRadianceSpecular() const
    {
        return g_radiance_specular_;
    }

    [[nodiscard]] RHIResourceRef<RHIImage> GetSpecAlbedo() const
    {
        return g_spec_albedo_;
    }

private:
    struct HandoffWindow
    {
        bool applies;
        float start;
        float end;
    };

    [[nodiscard]] HandoffWindow ComputeHandoffWindow() const;

    RHIResourceRef<RHIImage> CreateFullScreenTexture(PixelFormat format, const std::string &name);

    void AllocateGBuffer();

    void RenderReblur(const Vector3UInt &dispatch, const Vector3UInt &group);

    struct StageTiming
    {
        double sum_ms = 0.0;
        uint32_t count = 0;
    };

    // a frame slot's GPU time belongs to the previous submission that used that slot, so each frame
    // harvests the slot's pending times before recording which stages it dispatches into the slot
    void SampleGpuTimings(bool will_run_reblur);

    void LogGpuTimings(const char *tag) const;

    struct ConfigSnapshot
    {
        bool enabled;
        bool stabilization;
        bool radiance_fp16;
        NrdDebugMode debug_mode;
    };

    ConfigSnapshot config_;
    nrd::Instance *instance_ = nullptr;
    std::unique_ptr<RHINrdBackend> backend_;

    bool enabled_resources_ready_ = false;
    bool enabled_resources_failed_ = false;
    bool needs_gbuffer_ = true;
    bool reset_history_ = true;
    uint32_t frame_index_ = 0;
    uint32_t cumulated_samples_ = 0;
    uint32_t last_cumulated_samples_ = 0;
    uint32_t max_sample_per_pixel_ = 0;
    float far_plane_ = 1000.f;

    Mat4 view_matrix_ = Mat4::Identity();
    Mat4 projection_matrix_ = Mat4::Identity();
    Mat4 prev_view_matrix_ = Mat4::Identity();
    Mat4 prev_projection_matrix_ = Mat4::Identity();

    RHIResourceRef<RHIImage> input_;
    RHIResourceRef<RHIImage> output_;
    RHIResourceRef<RHIImage> output_history_;

    RHIResourceRef<RHIImage> g_radiance_;
    RHIResourceRef<RHIImage> g_normal_depth_;
    RHIResourceRef<RHIImage> g_albedo_obj_;
    RHIResourceRef<RHIImage> g_motion_;
    RHIResourceRef<RHIImage> g_radiance_specular_;
    RHIResourceRef<RHIImage> g_spec_albedo_;

    // NRD user-facing textures (IN_* written by nrd_pack, OUT_* written by ReBLUR)
    RHIResourceRef<RHIImage> in_mv_;
    RHIResourceRef<RHIImage> in_normal_roughness_;
    RHIResourceRef<RHIImage> in_viewz_;
    RHIResourceRef<RHIImage> in_diff_;
    RHIResourceRef<RHIImage> in_spec_;
    RHIResourceRef<RHIImage> out_diff_;
    RHIResourceRef<RHIImage> out_spec_;
    RHIResourceRef<RHIImage> validation_;

    RHIResourceRef<RHIShader> pack_shader_;
    RHIResourceRef<RHIPipelineState> pack_pipeline_;
    RHIResourceRef<RHIComputePass> pack_pass_;
    RHIResourceRef<RHIBuffer> pack_ubo_;

    RHIResourceRef<RHIShader> resolve_shader_;
    RHIResourceRef<RHIPipelineState> resolve_pipeline_;
    RHIResourceRef<RHIComputePass> resolve_pass_;
    RHIResourceRef<RHIBuffer> resolve_ubo_;

    // wraps the backend's RunDispatches so the ReBLUR block is timed like any engine pass
    RHIResourceRef<RHIComputePass> reblur_pass_;

    // refilled in place every ReBLUR frame; the dispatch layout is frame-invariant, so steady state
    // allocates nothing
    std::vector<RHINrdBackend::Dispatch> seam_dispatches_;
    std::vector<RHINrdBackend::DispatchResource> seam_resources_;

    StageTiming pack_timing_;
    StageTiming reblur_timing_;
    StageTiming resolve_timing_;
    uint32_t last_timing_log_count_ = 0;
    std::vector<uint8_t> slot_ran_reblur_;
    std::vector<uint8_t> slot_ran_resolve_;
};
} // namespace sparkle
