#pragma once

#if FRAMEWORK_APPLE

#include "rhi/RHIDenoiser.h"

#include <memory>

namespace sparkle
{
class MetalFxDenoiser final : public RHIDenoiser
{
public:
    MetalFxDenoiser(RHIContext *rhi, const RHIDenoiserDesc &desc);

    ~MetalFxDenoiser() override;

    [[nodiscard]] bool IsReady() const override;

    [[nodiscard]] bool NeedsInputs() const override;

    [[nodiscard]] const char *GetName() const override;

    [[nodiscard]] RHIResourceRef<RHIImage> GetOutput() const override;

    void UpdateFrameData(const RHIDenoiserFrameData &frame) override;

    bool Encode(const RHIDenoiserInputs &inputs) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sparkle

#endif
