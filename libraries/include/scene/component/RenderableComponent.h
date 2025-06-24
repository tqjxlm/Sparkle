#pragma once

#include "scene/component/Component.h"

namespace sparkle
{
class RenderProxy;

class RenderableComponent : public Component
{
public:
    RenderableComponent();

    ~RenderableComponent() override;

    [[nodiscard]] RenderProxy *GetRenderProxy() const
    {
        return render_proxy_;
    }

    void OnTransformChange() override;

    void DestroyRenderProxy();

    virtual void RecreateRenderProxy();

private:
    [[nodiscard]] virtual std::unique_ptr<RenderProxy> CreateRenderProxy() = 0;

    RenderProxy *render_proxy_ = nullptr;
};
} // namespace sparkle
