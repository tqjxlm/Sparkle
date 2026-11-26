#pragma once

#include "renderer/upscaler/Upscaler.h"

#include <memory>

struct FfxFsr3UpscalerContext;

namespace sparkle
{
class FfxBackend;
class RHIBuffer;
class RHIComputePass;
class RHIContext;
class RHIPipelineState;
class RHIShader;

// AMD FSR 3.1 temporal upscaler, run by FfxBackend on the engine's RHI. A compute pass turns the path tracer's view
// distance into device depth; FSR reads the color, that depth and the guides' motion vectors at input size and writes
// its output at output size.
class FsrUpscaler final : public Upscaler
{
public:
    FsrUpscaler(RHIContext *rhi, UpscalerDesc desc);

    ~FsrUpscaler() override;

    [[nodiscard]] bool IsReady() const override
    {
        return ready_;
    }

    [[nodiscard]] const char *GetName() const override
    {
        return "FSR";
    }

    [[nodiscard]] RHIResourceRef<RHIImage> GetOutput() const override
    {
        return output_;
    }

    void UpdateFrameData(const UpscalerFrameData &frame) override;

    void Resize(const Vector2UInt &input_size, const Vector2UInt &output_size) override;

    [[nodiscard]] RGTexture AddTo(RenderGraph &graph, const UpscalerInputs &inputs) override;

private:
    // (re)creates the FSR context and every image at desc_'s extents; ready_ tells whether that succeeded
    void Create();

    void Destroy();

    void Encode(RGExternalContext &context, const UpscalerInputs &inputs);

    RHIContext *rhi_;
    UpscalerDesc desc_;
    UpscalerFrameData frame_;
    // a reset requested on a frame FSR does not run carries over to the next one it does
    bool reset_history_ = true;
    bool ready_ = false;

    std::unique_ptr<FfxBackend> backend_;
    std::unique_ptr<FfxFsr3UpscalerContext> context_;

    RHIResourceRef<RHIImage> output_;
    RHIResourceRef<RHIImage> depth_;
    // FSR's shared resources, which its API leaves to the application
    RHIResourceRef<RHIImage> dilated_depth_;
    RHIResourceRef<RHIImage> dilated_motion_vectors_;
    RHIResourceRef<RHIImage> reconstructed_previous_nearest_depth_;

    RHIResourceRef<RHIShader> depth_shader_;
    RHIResourceRef<RHIPipelineState> depth_pipeline_;
    RHIResourceRef<RHIComputePass> depth_pass_;
    RHIResourceRef<RHIBuffer> depth_ubo_;
};
} // namespace sparkle
