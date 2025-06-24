#pragma once

#include "io/scene/SceneLoader.h"

namespace sparkle
{
class USDLoader : public SceneLoader
{
public:
    ~USDLoader() override = default;

    std::shared_ptr<SceneNode> Load(const std::string &path, Scene *scene) override;
};
} // namespace sparkle
