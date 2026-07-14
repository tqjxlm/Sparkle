#include "renderer/resource/IblCookPlan.h"

#include "core/Exception.h"
#include "renderer/resource/IblBrdfCookJob.h"
#include "renderer/resource/IblEnvCookJobs.h"

namespace sparkle
{
void IblCookPlan::CollectSceneIndependentJobs(std::vector<std::unique_ptr<CookJob>> &jobs)
{
    jobs.push_back(std::make_unique<IblBrdfCookJob>());
}

void IblCookPlan::CollectEnvironmentJobs(const std::shared_ptr<const Image2DCube> &environment,
                                         std::vector<std::unique_ptr<CookJob>> &jobs)
{
    ASSERT(environment);
    jobs.push_back(std::make_unique<IblDiffuseCookJob>(environment));
    jobs.push_back(std::make_unique<IblSpecularCookJob>(environment));
}
} // namespace sparkle
