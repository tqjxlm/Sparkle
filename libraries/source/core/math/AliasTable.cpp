#include "core/math/AliasTable.h"

#include <numeric>

namespace sparkle
{
std::vector<AliasTableEntry> BuildAliasTable(std::span<const float> weights)
{
    const double total = std::accumulate(weights.begin(), weights.end(), 0.0);
    if (total <= 0.0)
    {
        return {};
    }

    const auto count = static_cast<uint32_t>(weights.size());
    std::vector<AliasTableEntry> table(count);
    std::vector<double> scaled(count);
    std::vector<uint32_t> small;
    std::vector<uint32_t> large;

    for (uint32_t i = 0; i < count; i++)
    {
        table[i] = {.threshold = 1.f, .alias = i, .probability = static_cast<float>(weights[i] / total)};
        scaled[i] = weights[i] / total * count;
        (scaled[i] < 1.0 ? small : large).push_back(i);
    }

    while (!small.empty() && !large.empty())
    {
        const uint32_t less = small.back();
        small.pop_back();
        const uint32_t more = large.back();

        table[less].threshold = static_cast<float>(scaled[less]);
        table[less].alias = more;

        scaled[more] -= 1.0 - scaled[less];
        if (scaled[more] < 1.0)
        {
            large.pop_back();
            small.push_back(more);
        }
    }

    // what is left holds a scaled weight of 1 up to rounding, and keeps its default threshold of 1
    return table;
}
} // namespace sparkle
