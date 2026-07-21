#include "scene/cook/SceneCooker.h"

#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/cook/CookArtifactStore.h"
#include "core/cook/Cooker.h"
#include "core/task/TaskManager.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/material/MaterialManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace sparkle
{
namespace
{
std::vector<std::string> ReadCookList()
{
    auto content =
        FileManager::GetNativeFileManager()->ReadAsType<std::string>(Path::Resource("config/cook_list.json"));
    auto json = nlohmann::json::parse(content, nullptr, false);
    if (json.is_discarded())
    {
        return {};
    }

    std::vector<std::string> scenes;
    for (const auto &scene : json.value("scenes", nlohmann::json::array()))
    {
        scenes.push_back(scene.get<std::string>());
    }
    return scenes;
}

void PumpMainThreadUntil(const std::function<bool()> &done)
{
    while (!done())
    {
        TaskDispatcher::Instance().RunQueuedTasks(ThreadName::Main);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
} // namespace

std::vector<std::string> SceneCooker::GetCookList(const std::string &scene_override)
{
    if (!scene_override.empty())
    {
        // ad-hoc override for local iteration; a release cook uses the bundled list
        return {scene_override};
    }
    return ReadCookList();
}

int SceneCooker::Run(const std::string &scene_override, const JobPlan &job_plan, const JobAccelerator &accelerator)
{
    if (!job_plan.IsValid())
    {
        Log(Error, "cannot cook without a complete job plan");
        return 1;
    }

    auto scenes = GetCookList(scene_override);
    if (scenes.empty())
    {
        Log(Error, "nothing to cook: no scene argument and no cook list");
        return 1;
    }

    auto material_manager = MaterialManager::CreateInstance();

    // every requested job runs on its own dedicated thread, and a texture encode
    // transiently holds a full-resolution float image (~16 B/texel, ~270 MB for one 4k
    // source), so in-flight cooks are bounded instead of scaling with content
    constexpr size_t MaxInFlightCooks = 4;

    bool failed = false;
    size_t job_count = 0;
    auto request_all = [&](std::vector<std::unique_ptr<CookJob>> &jobs, std::vector<CookHandle> &handles) {
        for (auto &job : jobs)
        {
            job_count++;
            const auto key = MakeCookArtifactKey(*job);
            if (accelerator && CookArtifactStore::Load(key).empty())
            {
                auto result = accelerator(*job);
                if (!result.IsUnsupported())
                {
                    if (!result.IsSuccess() || result.GetPayload().empty())
                    {
                        Log(Error, "accelerated cook failed {}: {}", key.type, key.source_name);
                        failed = true;
                    }
                    else if (!CookArtifactStore::Save(key, result.GetPayload()))
                    {
                        failed = true;
                    }
                    continue;
                }
            }
            handles.push_back(Cooker::Request(std::move(job), [&failed](CookResult result) {
                if (!result.IsSuccess())
                {
                    failed = true;
                }
            }));
            PumpMainThreadUntil([&handles] {
                return static_cast<size_t>(std::ranges::count_if(handles, [](const CookHandle &handle) {
                           return !handle.OnDelivered()->IsReady();
                       })) < MaxInFlightCooks;
            });
        }
    };

    std::vector<CookHandle> handles;
    {
        std::vector<std::unique_ptr<CookJob>> jobs;
        if (!job_plan.collect_scene_independent_jobs(jobs))
        {
            Log(Error, "failed to collect scene-independent cook jobs");
            failed = true;
        }
        request_all(jobs, handles);
    }

    for (const auto &scene_file : scenes)
    {
        Log(Info, "cooking scene: {}", scene_file);

        auto scene = std::make_unique<Scene>();
        auto load_task = SceneManager::LoadScene(scene.get(), Path::Resource(scene_file), false, false);
        PumpMainThreadUntil([&] { return load_task->IsReady() && !scene->HasPendingAsyncTasks(); });

        std::vector<std::unique_ptr<CookJob>> jobs;
        if (!load_task->Get())
        {
            Log(Error, "failed to load cook root: {}", scene_file);
            failed = true;
        }
        else if (!scene->DidAsyncTasksSucceed())
        {
            Log(Error, "failed to resolve asynchronous resources for cook root: {}", scene_file);
            failed = true;
        }
        else if (!job_plan.collect_scene_jobs(*scene, jobs))
        {
            Log(Error, "failed to collect all cook jobs for scene: {}", scene_file);
            failed = true;
        }
        request_all(jobs, handles);
        PumpMainThreadUntil([&] {
            return std::ranges::all_of(handles,
                                       [](const CookHandle &handle) { return handle.OnDelivered()->IsReady(); });
        });
    }

    if (failed)
    {
        Log(Error, "cook failed: {} scene(s), {} job(s)", scenes.size(), job_count);
    }
    else
    {
        Log(Info, "cook finished: {} scene(s), {} job(s)", scenes.size(), job_count);
    }

    material_manager->Destroy();
    return failed ? 1 : 0;
}
} // namespace sparkle
