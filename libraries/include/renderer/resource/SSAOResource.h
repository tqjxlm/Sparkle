#pragma once

#include "core/math/Sampler.h"
#include "core/math/Types.h"

namespace sparkle
{
struct SSAOConfig
{
    static constexpr int SSAOSampleCount = 16;

    std::array<Vector4, SSAOSampleCount> samples;
    float scale = 1.0f;
    float threashold = 0.01f;

    SSAOConfig()
    {
        for (auto &sample : samples)
        {
            sample.head<3>() = sampler::UniformHemiSphere::Sample();
        }
    }
};
} // namespace sparkle
