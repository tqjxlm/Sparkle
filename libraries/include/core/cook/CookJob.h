#pragma once

#include "core/cook/CookArtifact.h"

#include <cstdint>
#include <string>

namespace sparkle
{
// one deterministic cook work unit. Execute runs on a worker thread and must stay CPU-only
// (no RHI, no scene access) so it can run in a render-less cook process.
class CookJob
{
public:
    virtual ~CookJob() = default;

    // stable family name, also the artifact directory under cooked/
    [[nodiscard]] virtual const char *GetType() const = 0;

    // bump to invalidate artifacts when the algorithm or the payload layout changes
    [[nodiscard]] virtual uint32_t GetVersion() const = 0;

    [[nodiscard]] virtual std::string GetSourceName() const = 0;

    [[nodiscard]] virtual uint32_t GetSourceHash() const = 0;

    [[nodiscard]] virtual CookJobResult Execute() = 0;

    // [0, 1] during Execute, negative if unknown
    [[nodiscard]] virtual float GetProgress() const
    {
        return -1.f;
    }
};

[[nodiscard]] inline CookArtifactKey MakeCookArtifactKey(const CookJob &job)
{
    return {.type = job.GetType(),
            .version = job.GetVersion(),
            .source_name = job.GetSourceName(),
            .source_hash = job.GetSourceHash()};
}
} // namespace sparkle
