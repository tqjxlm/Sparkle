#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace sparkle
{
// one slot of Vose's alias table: slot i yields i when a uniform fraction is below threshold, alias otherwise.
// probability is the chance of yielding i. layout must match AliasTableEntry in shaders/include/ray_trace.h.slang
struct AliasTableEntry
{
    float threshold;
    uint32_t alias;
    float probability;
};

// an alias table that picks each index with a probability proportional to its weight. empty if no weight is positive
std::vector<AliasTableEntry> BuildAliasTable(std::span<const float> weights);
} // namespace sparkle
