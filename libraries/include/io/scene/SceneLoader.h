#pragma once

#include "core/Path.h"

#include <memory>
#include <utility>

namespace sparkle
{
class SceneNode;
class Scene;

class SceneLoader
{
public:
    explicit SceneLoader(Path asset_root) : asset_root_(std::move(asset_root))
    {
    }

    virtual ~SceneLoader() = default;

    virtual std::shared_ptr<SceneNode> Load(Scene *scene) = 0;

protected:
    Path asset_root_;
};
} // namespace sparkle
