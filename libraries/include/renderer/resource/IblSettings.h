#pragma once

#include <cstdint>

namespace sparkle
{
// Algorithm settings shared by the CPU and GPU IBL producers. They belong to the
// derived resource itself, not to either producer or to a scene/render proxy.
struct IblSettings
{
    static constexpr uint32_t BrdfMapSize = 512;
    static constexpr uint32_t DiffuseMapSize = 512;
    static constexpr uint32_t SpecularMapSize = 1024;
    static constexpr uint8_t SpecularMipLevelCount = 5;
    static constexpr uint32_t TargetSampleCount = 2048;
    static constexpr float MaxEnvironmentBrightness = 10.f;
};
} // namespace sparkle
