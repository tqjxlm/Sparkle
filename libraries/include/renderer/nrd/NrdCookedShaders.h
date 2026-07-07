#pragma once

#include "rhi/RHINrdBackend.h"

#include <string>
#include <vector>

namespace sparkle
{
// Cooked ReBLUR shaders loaded from packed resources (produced by --test_case nrd_cook, committed
// under shaders/nrd/cooked). Pipelines are in nrd::InstanceDesc order; the version triple guards
// against a stale cook after an NRD submodule update.
struct NrdCookedShaders
{
    uint32_t version_major = 0;
    uint32_t version_minor = 0;
    uint32_t version_build = 0;
    std::vector<std::string> identifiers;
    std::vector<RHINrdBackend::CookedPipeline> pipelines;
};

bool LoadNrdCookedShaders(NrdCookedShaders &out);
} // namespace sparkle
