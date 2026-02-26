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

    void Dispatch(const FrontEndInputs &inputs, const RHIResourceRef<RHIImage> &denoised_output);

private:
    RHIContext *rhi_ = nullptr;
    Vector2UInt image_size_{};

    RHIResourceRef<RHIShader> passthrough_shader_;
    RHIResourceRef<RHIPipelineState> pipeline_state_;
    RHIResourceRef<RHIComputePass> compute_pass_;
    RHIResourceRef<RHIBuffer> uniform_buffer_;
};
} // namespace sparkle
