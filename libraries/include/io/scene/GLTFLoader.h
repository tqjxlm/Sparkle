#pragma once

#include "io/scene/SceneLoader.h"

namespace tinygltf
{
struct TinyGLTF;
}

namespace sparkle
{
class GLTFLoader : public SceneLoader
{
public:
    GLTFLoader();

    ~GLTFLoader() override;

    std::shared_ptr<SceneNode> Load(const std::string &path, Scene *scene) override;

private:
    std::shared_ptr<tinygltf::TinyGLTF> loader_;
};
} // namespace sparkle
