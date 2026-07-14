#pragma once

#include "core/cook/CookArtifact.h"

namespace sparkle
{
class CookJob;
class RHIContext;
struct RenderConfig;

// GPU producer for IBL-derived payloads. It is part of the IBL resource implementation
// and knows nothing about artifact lookup, manifests or persistence.
class IblCookAccelerator
{
public:
    [[nodiscard]] static CookJobResult TryCook(const CookJob &job, RHIContext *rhi, const RenderConfig &config);
};
} // namespace sparkle
