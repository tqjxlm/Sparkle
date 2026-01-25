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
    explicit GLTFLoader(Path asset_root);

    ~GLTFLoader() override;

    std::shared_ptr<SceneNode> Load(Scene *scene) override;

private:
    std::shared_ptr<tinygltf::TinyGLTF> loader_;
};
} // namespace sparkle
