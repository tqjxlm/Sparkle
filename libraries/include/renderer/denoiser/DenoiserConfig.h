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

struct DenoiserConfig : public ConfigCollection
{
    static DenoiserConfig &Get();

    void Init();

    DenoiserProvider provider = DenoiserProvider::Off;
    bool radiance_fp16 = true;
    bool metalfx_sync_init = false;

protected:
    void Validate() override
    {
    }
};
} // namespace sparkle
