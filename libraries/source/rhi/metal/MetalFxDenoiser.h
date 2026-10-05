#pragma once

#if FRAMEWORK_APPLE

#include "renderer/denoiser/Denoiser.h"

#include <memory>

namespace sparkle
{
class MetalFxDenoiser final : public Denoiser
{
public:
    MetalFxDenoiser(RHIContext *rhi, const DenoiserDesc &desc);

    ~MetalFxDenoiser() override;

    [[nodiscard]] bool IsReady() const override;

    [[nodiscard]] bool NeedsInputs() const override;

    [[nodiscard]] const char *GetName() const override;

    [[nodiscard]] RHIResourceRef<RHIImage> GetOutput() const override;

    void UpdateFrameData(const DenoiserFrameData &frame) override;

    void Resize(const Vector2UInt &input_size, const Vector2UInt &output_size) override;

    [[nodiscard]] RGTexture AddTo(RenderGraph &graph, const DenoiserInputs &inputs) override;

private:
    void Encode(RGExternalContext &pass_context, const DenoiserInputs &inputs, float handoff_weight);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sparkle

#endif
