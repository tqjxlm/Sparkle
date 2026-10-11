#pragma once

#include "io/Mesh.h"
#include "scene/Scene.h"
#include "scene/component/primitive/MeshPrimitive.h"

#include <string_view>

namespace sparkle
{
// the last primitive in the scene whose mesh resource has this name, or null
inline MeshPrimitive *FindMeshPrimitive(Scene *scene, std::string_view name)
{
    MeshPrimitive *result = nullptr;
    scene->GetRootNode()->Traverse([name, &result](SceneNode *node) {
        for (const auto &component : node->GetComponents())
        {
            auto *primitive = dynamic_cast<MeshPrimitive *>(component.get());
            if (primitive && primitive->GetMeshResource()->name == name)
            {
                result = primitive;
            }
        }
    });
    return result;
}
} // namespace sparkle
