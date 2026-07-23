#pragma once

#include "renderer/nrd/NrdConfig.h"

#include "core/math/Types.h"
#include "rhi/RHIBuffer.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIDenoiser.h"
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
class RHIContext;

// NVIDIA NRD (ReBLUR_DIFFUSE_SPECULAR) denoiser for the GPU path tracer. The provider-neutral
// path-tracing inputs are borrowed for each Encode call; this class owns only NRD resources.
class NrdDenoiser final : public RHIDenoiser
{
public:
    NrdDenoiser(RHIContext *rhi, const RHIDenoiserDesc &desc);

    ~NrdDenoiser() override;

    [[nodiscard]] bool IsReady() const override
    {
        return enabled_resources_ready_;
    }

    [[nodiscard]] bool NeedsInputs() const override
    {
        return needs_inputs_;
    }

    [[nodiscard]] const char *GetName() const override
    {
        return "NRD";
    }

    [[nodiscard]] RHIResourceRef<RHIImage> GetOutput() const override
    {
        return output_;
    }

    void UpdateFrameData(const RHIDenoiserFrameData &frame) override;

    bool Encode(const RHIDenoiserInputs &inputs) override;

private:
    struct HandoffWindow
    {
        bool applies;
        float start;
        float end;
    };

    struct StageTiming
    {
        double sum_ms = 0.0;
        uint32_t count = 0;
    };

    struct ConfigSnapshot
    {
        bool stabilization;
        NrdDebugMode debug_mode;
    };

    void SampleConfig();

    void Initialize();

    void EnsureOutputResources(PixelFormat format);

    void BindInputs(const RHIDenoiserInputs &inputs);

    [[nodiscard]] RHIResourceRef<RHIImage> CreateFullScreenTexture(PixelFormat format, const std::string &name) const;

    [[nodiscard]] HandoffWindow ComputeHandoffWindow() const;

    void RenderReblur(const RHIDenoiserInputs &inputs, const Vector3UInt &dispatch, const Vector3UInt &group);

    // A frame slot's GPU time belongs to the previous submission that used that slot, so each frame
    // harvests the slot's pending times before recording which stages it dispatches into the slot.
    void SampleGpuTimings(bool will_run_reblur);

    void LogGpuTimings(const char *tag) const;

    RHIContext *rhi_;
    Vector2UInt input_size_;
    ConfigSnapshot config_;
    nrd::Instance *instance_ = nullptr;
    std::unique_ptr<RHINrdBackend> backend_;

    bool enabled_resources_ready_ = false;
    bool enabled_resources_failed_ = false;
    bool needs_inputs_ = true;
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

    RHIResourceRef<RHIImage> output_;
    RHIResourceRef<RHIImage> output_history_;

    // NRD user-facing textures (IN_* written by nrd_pack, OUT_* written by ReBLUR).
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

    // Wraps the backend's RunDispatches so the ReBLUR block is timed like any engine pass.
    RHIResourceRef<RHIComputePass> reblur_pass_;

    // Refilled in place every ReBLUR frame; the dispatch layout is frame-invariant, so steady state
    // allocates nothing.
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
