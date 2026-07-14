#pragma once

#include "core/cook/CookArtifact.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sparkle
{
class CookJob;
class Scene;

// Composes scene loading and an explicitly supplied domain plan above the core artifact
// store. Runtime scene objects do not expose build hooks. An optional accelerator may
// synchronously execute jobs it supports; only Unsupported falls back to CPU Execute().
class SceneCooker
{
public:
    struct JobPlan
    {
        std::function<bool(std::vector<std::unique_ptr<CookJob>> &)> collect_scene_independent_jobs;
        std::function<bool(const Scene &, std::vector<std::unique_ptr<CookJob>> &)> collect_scene_jobs;

        [[nodiscard]] bool IsValid() const
        {
            return collect_scene_independent_jobs && collect_scene_jobs;
        }
    };

    using JobAccelerator = std::function<CookJobResult(const CookJob &)>;

    [[nodiscard]] static std::vector<std::string> GetCookList(const std::string &scene_override);

    static int Run(const std::string &scene_override, const JobPlan &job_plan, const JobAccelerator &accelerator = {});
};
} // namespace sparkle
