#pragma once

#include <memory>
#include <vector>

namespace sparkle
{
class CookJob;
class Image2DCube;

// Declares the complete IBL contribution to a build cook. Job ownership stays in
// the IBL domain; application composition decides which scene resources feed it.
class IblCookPlan
{
public:
    static void CollectSceneIndependentJobs(std::vector<std::unique_ptr<CookJob>> &jobs);

    static void CollectEnvironmentJobs(const std::shared_ptr<const Image2DCube> &environment,
                                       std::vector<std::unique_ptr<CookJob>> &jobs);
};
} // namespace sparkle
