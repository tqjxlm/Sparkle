#include "scene/component/primitive/PrimitiveComponent.h"

#include "core/task/TaskManager.h"
#include "renderer/proxy/PrimitiveRenderProxy.h"
#include "scene/Scene.h"
#include "scene/material/Material.h"

namespace sparkle
{
PrimitiveComponent::PrimitiveComponent(const Vector3 &center, const Vector3 &size)
    : local_bound_(center, size), world_bound_(local_bound_)
{
}

PrimitiveComponent::~PrimitiveComponent()
{
    if (!is_attached_)
    {
        return;
    }

    auto *scene = node_->GetScene();

    if (material_)
    {
        scene->UnregisterMaterial(material_);
    }
    scene->UnregisterPrimitive(this);
}

void PrimitiveComponent::Tick()
{
    Component::Tick();
}

void PrimitiveComponent::OnAttach()
{
    Component::OnAttach();

    auto *scene = node_->GetScene();

    scene->RegisterPrimitive(this);

    // it will need a material to be able to actually render
    if (material_)
    {
        scene->RegisterMaterial(material_.get());

        TaskManager::RunInRenderThread([this]() { RecreateRenderProxy(); });
    }
}

void PrimitiveComponent::OnTransformChange()
{
    RenderableComponent::OnTransformChange();

    world_bound_ = local_bound_.TransformTo(GetTransform());
}

void PrimitiveComponent::SetMaterial(const std::shared_ptr<Material> &material)
{
    if (material == material_)
    {
        return;
    }

    if (!is_attached_)
    {
        material_ = material;

        // a component that is not attached to a scene has no rendering resources to refresh
        return;
    }

    auto *scene = node_->GetScene();
    scene->UnregisterMaterial(material_);
    scene->RegisterMaterial(material.get());

    material_ = material;

    TaskManager::RunInRenderThread([this]() { RecreateRenderProxy(); });
}

void PrimitiveComponent::RecreateRenderProxy()
{
    RenderableComponent::RecreateRenderProxy();

    GetRenderProxy()->As<PrimitiveRenderProxy>()->SetMaterialRenderProxy(material_->GetRenderProxy());
}
} // namespace sparkle
