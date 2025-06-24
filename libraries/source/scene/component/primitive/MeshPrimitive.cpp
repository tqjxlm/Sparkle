#include "scene/component/primitive/MeshPrimitive.h"

#include "io/Mesh.h"
#include "renderer/proxy/MeshRenderProxy.h"

namespace sparkle
{
MeshPrimitive::MeshPrimitive(std::shared_ptr<Mesh> mesh)
    : PrimitiveComponent(mesh->center, mesh->extent), raw_mesh_(std::move(mesh))
{
}

MeshPrimitive::~MeshPrimitive() = default;

std::unique_ptr<RenderProxy> MeshPrimitive::CreateRenderProxy()
{
    return std::make_unique<MeshRenderProxy>(raw_mesh_, node_->GetName(), local_bound_);
}
} // namespace sparkle
