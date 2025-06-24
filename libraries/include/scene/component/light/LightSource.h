#pragma once

#include "scene/component/RenderableComponent.h"

namespace sparkle
{
class LightSourceComponent : public RenderableComponent
{
public:
    LightSourceComponent();

    ~LightSourceComponent() override;

    void OnAttach() override;
};
} // namespace sparkle
