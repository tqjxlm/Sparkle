#pragma once

#include "renderer/pass/IBLPass.h"

namespace sparkle
{
class IBLSpecularPass : public IBLPass
{
public:
    using IBLPass::IBLPass;

    ~IBLSpecularPass() override;

    void CookOnTheFly(const RenderConfig &config) override;

    void InitRenderResources(const RenderConfig &config) override;

    void Render() override;

protected:
    RHIResourceRef<RHIImage> CreateIBLMap(bool for_cooking, bool allow_write) override;

    [[nodiscard]] std::string GetCachePath() const override;

    // update level-related resources and reset progress
    void StartCacheLevel(uint8_t level);

private:
    uint8_t current_caching_level_ = 0;

    static constexpr int IblMapSize = 1024;
};
} // namespace sparkle
