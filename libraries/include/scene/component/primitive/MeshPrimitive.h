#pragma once

#include "scene/component/primitive/PrimitiveComponent.h"

namespace sparkle
{
struct Mesh;

class MeshPrimitive : public PrimitiveComponent
{
public:
    explicit MeshPrimitive(std::shared_ptr<Mesh> mesh);

    ~MeshPrimitive() override;

    [[nodiscard]] const std::shared_ptr<Mesh> &GetMeshResource() const
    {
        return raw_mesh_;
    }

protected:
    std::unique_ptr<RenderProxy> CreateRenderProxy() override;

    std::shared_ptr<Mesh> raw_mesh_;
};
} // namespace sparkle
