#include "scene/component/primitive/SpherePrimitive.h"

#include "core/task/TaskManager.h"
#include "io/Mesh.h"
#include "renderer/proxy/SphereRenderProxy.h"

namespace sparkle
{
SpherePrimitive::SpherePrimitive() : MeshPrimitive(Mesh::GetUnitSphere())
{
}

std::unique_ptr<RenderProxy> SpherePrimitive::CreateRenderProxy()
{
    auto proxy = std::make_unique<SphereRenderProxy>(raw_mesh_, node_->GetName(), local_bound_);

    proxy->SetRadius(radius_);

    return proxy;
}

void SpherePrimitive::OnTransformChange()
{
    MeshPrimitive::OnTransformChange();

    // it is a sphere and we do not allow non-uniform scaling
    // may support non-uniform scaling in the future
    radius_ = GetLocalTransform().GetScale().maxCoeff();

    TaskManager::RunInRenderThread([this]() {
        if (!GetRenderProxy())
        {
            return;
        }

        GetRenderProxy()->As<SphereRenderProxy>()->SetRadius(radius_);
    });
}
} // namespace sparkle
