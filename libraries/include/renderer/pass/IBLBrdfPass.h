#pragma once

#include "renderer/pass/IBLPass.h"

namespace sparkle
{
class IBLBrdfPass : public IBLPass
{
public:
    explicit IBLBrdfPass(RHIContext *ctx);

    ~IBLBrdfPass() override;

    void AddTo(RenderGraph &graph, unsigned samples_per_dispatch) override;

    void InitRenderResources(const RenderConfig &config) override;

protected:
    RHIResourceRef<RHIImage> CreateIBLMap(bool for_cooking, bool allow_write, PixelFormat resource_format) override;

private:
};
} // namespace sparkle
