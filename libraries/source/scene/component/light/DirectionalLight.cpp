#include "scene/component/light/DirectionalLight.h"

#include "core/TaskManager.h"
#include "core/math/Transform.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "scene/Scene.h"

namespace sparkle
{
DirectionalLight::DirectionalLight() = default;

void DirectionalLight::OnTransformChange()
{
    LightSourceComponent::OnTransformChange();

    Vector3 new_direction = GetTransform().TransformDirection(Front);

    TaskManager::RunInRenderThread([this, new_direction]() {
        if (!GetRenderProxy())
        {
            return;
        }

        static_cast<DirectionalLightRenderProxy *>(GetRenderProxy())->UpdateMatrices(new_direction);
    });
}

void DirectionalLight::SetColor(const Vector3 &color)
{
    color_ = color;

    TaskManager::RunInRenderThread([this, color]() {
        if (!GetRenderProxy())
        {
            return;
        }

        static_cast<DirectionalLightRenderProxy *>(GetRenderProxy())->SetColor(color);
    });
}

std::unique_ptr<RenderProxy> DirectionalLight::CreateRenderProxy()
{
    auto proxy = std::make_unique<DirectionalLightRenderProxy>();

    proxy->SetColor(color_);

    proxy->UpdateMatrices(GetTransform().TransformDirection(Front));

    if (node_->GetScene()->GetDirectionalLight() == this)
    {
        node_->GetScene()->GetRenderProxy()->SetDirectionalLight(proxy.get());
    }
    else
    {
        Log(Warn, "multiple directional lights detected, only the first one will take effect");
    }

    return proxy;
}

void DirectionalLight::OnAttach()
{
    LightSourceComponent::OnAttach();

    ASSERT(!node_->GetScene()->GetDirectionalLight());

    node_->GetScene()->SetDirectionalLight(this);
}
} // namespace sparkle
