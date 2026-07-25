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

    bool Encode(const DenoiserInputs &inputs) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sparkle

#endif
