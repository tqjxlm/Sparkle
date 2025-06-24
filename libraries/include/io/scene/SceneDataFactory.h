#pragma once

#include "scene/SceneNode.h"

#include <future>

namespace sparkle
{
// a model is a hierarchical set of meshes
class SceneDataFactory
{
public:
    static std::future<void> Load(const std::string &path, Scene *scene,
                                  std::function<void(const std::shared_ptr<SceneNode> &)> on_loaded_fn_main_thread,
                                  bool async = true);
};
} // namespace sparkle
