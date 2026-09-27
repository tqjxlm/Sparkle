#pragma once

#include "renderer/pass/IBLPass.h"

namespace sparkle
{
class IBLSpecularPass : public IBLPass
{
public:
    using IBLPass::IBLPass;

    ~IBLSpecularPass() override;

    void AddTo(RenderGraph &graph, unsigned samples_per_dispatch) override;

    void InitRenderResources(const RenderConfig &config) override;

protected:
    RHIResourceRef<RHIImage> CreateIBLMap(bool for_cooking, bool allow_write, PixelFormat resource_format) override;

    // starts cooking mip `level`
    void StartCacheLevel(uint8_t level);

private:
    uint8_t current_caching_level_ = 0;
};
} // namespace sparkle
