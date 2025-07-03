#include "io/scene/SceneDataFactory.h"

#include "core/Logger.h"
#include "core/TaskManager.h"
#include "io/scene/GLTFLoader.h"
#include "io/scene/USDLoader.h"

#include <filesystem>

namespace sparkle
{
std::future<void> SceneDataFactory::Load(
    const std::string &path, Scene *scene,
    std::function<void(const std::shared_ptr<SceneNode> &)> on_loaded_fn_main_thread, bool async)
{
    // io tasks must be intiated from main thread
    ASSERT(ThreadManager::IsInMainThread());

    auto task_promise = std::make_shared<std::promise<void>>();

    std::shared_ptr<SceneLoader> loader;

    std::filesystem::path file_path(path);
    if (file_path.extension() == ".gltf" || file_path.extension() == ".glb")
    {
        loader = std::make_shared<GLTFLoader>();
    }
    else if (file_path.extension() == ".usda" || file_path.extension() == ".usdc" || file_path.extension() == ".usdz")
    {
        loader = std::make_shared<USDLoader>();
    }
    else
    {
        Log(Error, "Unsupported model format: {}", path);
        task_promise->set_value();
        return task_promise->get_future();
    }

    if (async)
    {
        TaskManager::RunInWorkerThread([loader_moved = std::move(loader), scene, path, task_promise,
                                        on_loaded_fn = std::move(on_loaded_fn_main_thread)]() {
            auto loaded_root = loader_moved->Load(path, scene);

            // successful or not, we will mark the task as finished

            if (loaded_root)
            {
                Log(Debug, "Async model load ok: {}", path);
                // callback functions must run in main thread
                TaskManager::RunInMainThread([task_promise, on_loaded_fn_moved = std::move(on_loaded_fn),
                                              model_node = std::move(loaded_root)]() {
                    on_loaded_fn_moved(model_node);
                    task_promise->set_value();
                });
            }
            else
            {
                task_promise->set_value();
            }
        });
    }
    else
    {
        auto loaded_root = loader->Load(path, scene);
        if (loaded_root)
        {
            on_loaded_fn_main_thread(loaded_root);
        }
        task_promise->set_value();
    }

    return task_promise->get_future();
}
} // namespace sparkle
