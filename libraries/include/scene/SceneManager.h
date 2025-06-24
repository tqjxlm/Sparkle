#pragma once

#include <future>
#include <string>

namespace sparkle
{
class Scene;
struct RenderConfig;

class SceneManager
{
public:
    static std::future<void> LoadScene(Scene *scene, const std::string &scene_name);

    static void RemoveLastNode(Scene *scene);

    static void GenerateRandomSpheres(Scene &scene, unsigned count);

    static void AddDefaultSky(Scene *scene);

    static void AddDefaultDirectionalLight(Scene *scene);
};
} // namespace sparkle
