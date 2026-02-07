#pragma once

#include "core/Path.h"
#include "core/task/TaskFuture.h"

namespace sparkle
{
class Scene;
class UiManager;

class SceneManager
{
public:
    [[nodiscard]] static std::shared_ptr<TaskFuture<void>> LoadScene(Scene *scene, const Path &asset_path,
                                                                     bool need_default_sky, bool need_default_lighting);

    static void RemoveLastDebugSphere(Scene *scene);

    static void GenerateRandomSpheres(Scene &scene, unsigned count);

    [[nodiscard]] static std::shared_ptr<TaskFuture<void>> AddDefaultSky(Scene *scene);

    static void AddDefaultDirectionalLight(Scene *scene);

    static void DrawUi(Scene *scene, bool need_default_sky, bool need_default_lighting);
};
} // namespace sparkle
