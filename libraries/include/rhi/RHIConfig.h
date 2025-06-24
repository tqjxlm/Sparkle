#pragma once

#include "application/ConfigCollection.h"

namespace sparkle
{
struct RHIConfig : public ConfigCollection
{
    enum class ApiPlatform : uint8_t
    {
        None,
        Vulkan,
        Metal
    };

    ApiPlatform api_platform = ApiPlatform::None;
    bool use_vsync;
    uint32_t msaa_samples;
    bool enable_validation;
    bool enable_pre_transform;
    bool measure_gpu_time;

    void Init();

protected:
    void Validate() override;
};
} // namespace sparkle
