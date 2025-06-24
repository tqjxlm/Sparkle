#pragma once

#include "renderer/pass/IBLPass.h"

namespace sparkle
{
class IBLBrdfPass : public IBLPass
{
public:
    explicit IBLBrdfPass(RHIContext *ctx);

    ~IBLBrdfPass() override;

    void CookOnTheFly(const RenderConfig &config) override;

    void InitRenderResources(const RenderConfig &config) override;

    void Render() override;

protected:
    [[nodiscard]] std::string GetCachePath() const override;

    RHIResourceRef<RHIImage> CreateIBLMap(bool for_cooking, bool allow_write) override;

private:
    static constexpr int IblMapSize = 512;
};
} // namespace sparkle
