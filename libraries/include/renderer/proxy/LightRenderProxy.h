#pragma once

#include "core/RenderProxy.h"

namespace sparkle
{
class Ray;

class LightRenderProxy : public RenderProxy
{
public:
#pragma region CPU Render

    [[nodiscard]] virtual Vector3 Evaluate(const Ray &ray) const = 0;

    virtual void Sample(const Vector3 &origin, Vector3 &direction) const = 0;

#pragma endregion
};
} // namespace sparkle
