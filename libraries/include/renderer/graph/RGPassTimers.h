#pragma once

#include "rhi/RHIPass.h"

#include <string>
#include <unordered_map>

namespace sparkle
{
class RHIContext;

// the timed passes of render graph raster physical passes, by name (the member names joined by '+'). a graph lives one
// frame while a time arrives frames after its physical pass records, so the owner of the graphs keeps them across
// frames. physical passes sharing a name share a timer, which reports their last run.
class RGPassTimers
{
public:
    explicit RGPassTimers(RHIContext *rhi) : rhi_(rhi)
    {
    }

private:
    friend class RenderGraph;

    // the timed pass of raster physical passes named `name`, created on first use
    [[nodiscard]] RHIPass *Get(const std::string &name);

    RHIContext *rhi_;
    std::unordered_map<std::string, RHIResourceRef<RHIPass>> passes_;
};
} // namespace sparkle
