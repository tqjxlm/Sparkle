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

    // whether Vulkan copies each frame-recorded upload into a read-only texture again in a submission of its own after
    // the frame's
    enum class UploadReplay : uint8_t
    {
        // On on the Apple Paravirtual device, Off elsewhere
        Auto,
        On,
        Off
    };

    ApiPlatform api_platform = ApiPlatform::None;
    UploadReplay upload_replay = UploadReplay::Auto;
    bool use_vsync;
    bool enable_validation;
    bool enable_sync_validation;
    bool enable_pre_transform;
    bool measure_gpu_time;
    bool sampler_anisotropy;

    void Init();

protected:
    void Validate() override;
};
} // namespace sparkle
