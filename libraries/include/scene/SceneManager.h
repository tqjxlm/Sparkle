#pragma once

#include "core/task/TaskFuture.h"

#include <string>

namespace sparkle
{
class Scene;
class SceneNode;
struct RenderConfig;

class SceneManager
{
public:
    static std::shared_ptr<TaskFuture<>> LoadScene(Scene *scene, const std::string &scene_name);

    static void RemoveLastNode(Scene *scene);

    static void GenerateRandomSpheres(Scene &scene, unsigned count);

    static void AddDefaultSky(Scene *scene);

    static void AddDefaultDirectionalLight(Scene *scene);
};
} // namespace sparkle
