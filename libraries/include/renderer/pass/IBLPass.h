#pragma once

#include "renderer/pass/PipelinePass.h"

#include "renderer/resource/IblSettings.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"

#include <functional>

namespace sparkle
{
class IBLPass : public PipelinePass
{
public:
    IBLPass(RHIContext *ctx, const RHIResourceRef<RHIImage> &env_map, PixelFormat target_format);

    ~IBLPass() override;

    [[nodiscard]] RHIResourceRef<RHIImage> GetResource() const
    {
        if (!is_ready_)
        {
            return nullptr;
        }
        return ibl_image_;
    }

    [[nodiscard]] bool IsReady() const
    {
        return is_ready_;
    }

    virtual void CookOnTheFly(const RenderConfig &config, unsigned samples_per_dispatch) = 0;

    // Consume a payload produced by the matching CPU cook job. Render thread only.
    // Returns false when the payload does not match this pass's resource layout.
    bool ApplyArtifact(const std::vector<char> &payload);

    // Receives the compact payload after GPU generation. Persistence belongs to the
    // derived-resource coordinator or cook job, never to the GPU pass.
    void SetArtifactReadyCallback(std::function<void(std::vector<char>)> callback)
    {
        artifact_ready_callback_ = std::move(callback);
    }

protected:
    void Complete();

    void PrepareForCooking();

    virtual RHIResourceRef<RHIImage> CreateIBLMap(bool for_cooking, bool allow_write, PixelFormat resource_format) = 0;

    PixelFormat target_format_;

    RHIResourceRef<RHIImage> ibl_image_;

    RHIResourceRef<RHIImage> env_map_;

    RHIResourceRef<RHIRenderTarget> clear_target_;

    std::unique_ptr<class ClearTexturePass> clear_pass_;

    RHIResourceRef<RHIShader> compute_shader_;

    RHIResourceRef<RHIComputePass> compute_pass_;

    RHIResourceRef<RHIPipelineState> pipeline_state_;

    RHIResourceRef<RHIBuffer> cs_ub_;

    unsigned sample_count_ = 0;
    unsigned target_sample_count_ = IblSettings::TargetSampleCount;

private:
    void Finalize();

    // builds the resident IBL cube from a compressed artifact payload: the native compressed
    // cube, or an fp16 decode where the device cannot sample the format. null on a bad payload
    RHIResourceRef<RHIImage> MakeIblResource(const std::vector<char> &payload);

    bool is_ready_ = false;

    std::function<void(std::vector<char>)> artifact_ready_callback_;
};
} // namespace sparkle
