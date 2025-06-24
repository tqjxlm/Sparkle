#pragma once

#include <memory>
#include <string>

namespace sparkle
{
class SceneNode;
class Scene;

class SceneLoader
{
public:
    virtual ~SceneLoader() = default;

    virtual std::shared_ptr<SceneNode> Load(const std::string &path, Scene *scene) = 0;
};
} // namespace sparkle
