#pragma once

#include "scene/component/light/LightSource.h"

namespace sparkle
{
class DirectionalLight : public LightSourceComponent
{
public:
    DirectionalLight();

    std::unique_ptr<RenderProxy> CreateRenderProxy() override;

    void OnTransformChange() override;

    void SetColor(const Vector3 &color);

    void OnAttach() override;

private:
    Vector3 color_ = Ones;
};
} // namespace sparkle
