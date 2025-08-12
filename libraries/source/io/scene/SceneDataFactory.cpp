#include "io/scene/SceneDataFactory.h"

#include "core/Logger.h"
#include "core/task/TaskFuture.h"
#include "core/task/TaskManager.h"
#include "io/scene/GLTFLoader.h"
#include "io/scene/USDLoader.h"

#include <filesystem>

namespace sparkle
{
std::shared_ptr<TaskFuture<std::shared_ptr<SceneNode>>> SceneDataFactory::Load(const std::string &path, Scene *scene,
                                                                               bool async)
{
    // io tasks must be intiated from main thread
    ASSERT(ThreadManager::IsInMainThread());

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

        return TaskManager::Instance().EnqueueTask([]() { return std::shared_ptr<SceneNode>(nullptr); },
                                                   TargetThread::Current);
    }

    return TaskManager::Instance().EnqueueTask(
        [loader_moved = std::move(loader), path, scene]() { return loader_moved->Load(path, scene); },
        async ? TargetThread::Worker : TargetThread::Current);
}
} // namespace sparkle
