#pragma once

#include "scene/component/primitive/MeshPrimitive.h"

namespace sparkle
{
class SpherePrimitive : public MeshPrimitive
{
public:
    explicit SpherePrimitive();

    void OnTransformChange() override;

    std::unique_ptr<RenderProxy> CreateRenderProxy() override;

private:
    float radius_ = 1.f;
};
} // namespace sparkle
