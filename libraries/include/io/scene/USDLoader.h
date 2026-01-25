#pragma once

#include "io/scene/SceneLoader.h"

namespace sparkle
{
class USDLoader : public SceneLoader
{
public:
    using SceneLoader::SceneLoader;

    ~USDLoader() override = default;

    std::shared_ptr<SceneNode> Load(Scene *scene) override;
};
} // namespace sparkle
