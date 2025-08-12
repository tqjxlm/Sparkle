#pragma once

#include "core/task/TaskFuture.h"
#include "scene/SceneNode.h"

namespace sparkle
{
// a model is a hierarchical set of meshes
class SceneDataFactory
{
public:
    static std::shared_ptr<TaskFuture<std::shared_ptr<SceneNode>>> Load(const std::string &path, Scene *scene,
                                                                        bool async = true);
};
} // namespace sparkle
