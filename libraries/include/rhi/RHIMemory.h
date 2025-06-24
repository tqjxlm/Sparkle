#pragma once

#include "core/Enum.h"

namespace sparkle
{
enum class RHIMemoryProperty : uint8_t
{
    None = 0,
    // it means we can map gpu memory from cpu
    HostVisible = 1 << 0,
    // it means any change to the mapped memory will be automatically flushed to gpu
    HostCoherent = 1 << 1,
    // it means we have fast read access to the mapped memory
    HostCached = 1 << 2,
    // it means this memory is gpu only
    DeviceLocal = 1 << 3,
    // it means the memory is always mapped
    AlwaysMap = 1 << 4,
};

RegisterEnumAsFlag(RHIMemoryProperty);
} // namespace sparkle
