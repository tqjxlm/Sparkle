#pragma once

#include "core/Path.h"
#include "core/task/TaskFuture.h"
#include "scene/SceneNode.h"

namespace sparkle
{
// a model is a hierarchical set of meshes
class SceneDataFactory
{
public:
    static std::shared_ptr<TaskFuture<std::shared_ptr<SceneNode>>> Load(const Path &path, Scene *scene,
                                                                        bool async = true);
};
} // namespace sparkle
