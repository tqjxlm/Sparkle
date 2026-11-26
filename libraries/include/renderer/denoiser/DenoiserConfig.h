#pragma once

#include "application/ConfigCollection.h"

#include <cstdint>

namespace sparkle
{
enum class DenoiserProvider : uint8_t
{
    Off,
    Auto,
    Nrd,
    MetalFx
};

enum class UpscalerProvider : uint8_t
{
    Off,
    Auto,
    Fsr,
    MetalFx
};

struct DenoiserConfig : public ConfigCollection
{
    static DenoiserConfig &Get();

    void Init();

    DenoiserProvider provider = DenoiserProvider::Off;
    UpscalerProvider upscaler = UpscalerProvider::Off;
    bool radiance_fp16 = true;
    bool metalfx_sync_init = false;

protected:
    void Validate() override
    {
    }
};
} // namespace sparkle
