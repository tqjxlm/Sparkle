#include "io/scene/SceneDataFactory.h"

#include "core/Logger.h"
#include "core/task/TaskFuture.h"
#include "core/task/TaskManager.h"
#include "io/scene/GLTFLoader.h"
#include "io/scene/USDLoader.h"

#include <filesystem>

namespace sparkle
{
std::shared_ptr<TaskFuture<std::shared_ptr<SceneNode>>> SceneDataFactory::Load(const Path &path, Scene *scene,
                                                                               bool async)
{
    // io tasks must be intiated from main thread
    ASSERT(ThreadManager::IsInMainThread());

    std::shared_ptr<SceneLoader> loader;

    if (path.path.extension() == ".gltf" || path.path.extension() == ".glb")
    {
        loader = std::make_shared<GLTFLoader>(path);
    }
    else if (path.path.extension() == ".usda" || path.path.extension() == ".usdc" || path.path.extension() == ".usdz")
    {
        loader = std::make_shared<USDLoader>(path);
    }
    else
    {
        Log(Error, "Unsupported model format: {}", path.path.string());

        return TaskManager::Instance().EnqueueTask([]() { return std::shared_ptr<SceneNode>(nullptr); },
                                                   TargetThread::Current);
    }

    auto load_task = [loader_moved = std::move(loader), path, scene]() { return loader_moved->Load(scene); };

    // loaders fan out to the worker pool and wait on it, so they must not hold a pool worker
    if (async)
    {
        return TaskManager::RunInDedicatedThread(std::move(load_task));
    }

    return TaskManager::Instance().EnqueueTask(std::move(load_task), TargetThread::Current);
}
} // namespace sparkle
