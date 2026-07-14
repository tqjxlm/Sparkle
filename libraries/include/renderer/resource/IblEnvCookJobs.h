#pragma once

#include "core/cook/CookJob.h"
#include "renderer/resource/IblSettings.h"

#include <atomic>
#include <memory>

namespace sparkle
{
class Image2DCube;
class IblCookAccelerator;

// CPU producers of the environment-derived IBL artifacts. IBLDiffusePass and
// IBLSpecularPass produce the same payloads on the GPU; outputs must match (enforced by
// the ibl_parity test case). keep the math in sync with
// shaders/screen/ibl_{diffuse,specular}.cs.slang.
class IblEnvCookJob : public CookJob
{
public:
    explicit IblEnvCookJob(std::shared_ptr<const Image2DCube> env_map);

    [[nodiscard]] std::string GetSourceName() const override;

    [[nodiscard]] uint32_t GetSourceHash() const override
    {
        return env_hash_;
    }

protected:
    std::shared_ptr<const Image2DCube> env_map_;

    uint32_t env_hash_ = 0;

    std::atomic<uint32_t> cooked_rows_{0};

    friend class IblCookAccelerator;
};

class IblDiffuseCookJob : public IblEnvCookJob
{
public:
    static constexpr uint32_t MapSize = IblSettings::DiffuseMapSize;
    static constexpr uint32_t TargetSampleCount = IblSettings::TargetSampleCount;

    using IblEnvCookJob::IblEnvCookJob;

    [[nodiscard]] const char *GetType() const override
    {
        return "ibl_diffuse";
    }

    [[nodiscard]] uint32_t GetVersion() const override
    {
        return 1;
    }

    [[nodiscard]] float GetProgress() const override
    {
        return static_cast<float>(cooked_rows_.load()) / (6 * MapSize);
    }

    [[nodiscard]] CookJobResult Execute() override;
};

class IblSpecularCookJob : public IblEnvCookJob
{
public:
    static constexpr uint32_t MapSize = IblSettings::SpecularMapSize;
    static constexpr uint32_t TargetSampleCount = IblSettings::TargetSampleCount;
    static constexpr uint8_t MipLevelCount = IblSettings::SpecularMipLevelCount;

    using IblEnvCookJob::IblEnvCookJob;

    [[nodiscard]] const char *GetType() const override
    {
        return "ibl_specular";
    }

    [[nodiscard]] uint32_t GetVersion() const override
    {
        // version 2: the cross-backend payload layout was harmonized to mip-major
        return 2;
    }

    [[nodiscard]] float GetProgress() const override;

    [[nodiscard]] CookJobResult Execute() override;
};
} // namespace sparkle
