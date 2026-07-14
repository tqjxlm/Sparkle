#pragma once

#include "core/cook/CookJob.h"
#include "renderer/resource/IblSettings.h"

#include <atomic>

namespace sparkle
{
// CPU producer of the ibl_brdf artifact. IBLBrdfPass produces the same payload on the GPU;
// their outputs must match (enforced by the ibl_parity test case). keep the math in sync
// with shaders/screen/ibl_brdf.cs.slang.
class IblBrdfCookJob : public CookJob
{
public:
    static constexpr uint32_t MapSize = IblSettings::BrdfMapSize;
    static constexpr uint32_t TargetSampleCount = IblSettings::TargetSampleCount;

    [[nodiscard]] const char *GetType() const override
    {
        return "ibl_brdf";
    }

    [[nodiscard]] uint32_t GetVersion() const override
    {
        return 1;
    }

    [[nodiscard]] std::string GetSourceName() const override
    {
        return "brdf";
    }

    [[nodiscard]] uint32_t GetSourceHash() const override
    {
        return 0;
    }

    [[nodiscard]] float GetProgress() const override
    {
        return static_cast<float>(cooked_rows_.load()) / MapSize;
    }

    [[nodiscard]] CookJobResult Execute() override;

private:
    std::atomic<uint32_t> cooked_rows_{0};
};
} // namespace sparkle
