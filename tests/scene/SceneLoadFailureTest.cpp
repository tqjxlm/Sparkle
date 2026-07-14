#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/task/TaskManager.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/component/light/SkyLight.h"

namespace sparkle
{
class SceneLoadFailureTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        auto *scene = app.GetScene();

        switch (stage_)
        {
        case Stage::InsertRenderBarrier:
            render_barrier_ = TaskManager::RunInRenderThread([] {});
            stage_ = Stage::WaitForRenderBarrier;
            return Result::Pending;

        case Stage::WaitForRenderBarrier:
            if (!render_barrier_->IsReady())
            {
                return Result::Pending;
            }
            load_task_ = SceneManager::LoadScene(scene, Path::Resource("missing/scene_load_failure.usda"), true, false);
            stage_ = Stage::WaitForMissingScene;
            return Result::Pending;

        case Stage::WaitForMissingScene:
            if (!load_task_->IsReady() || scene->HasPendingAsyncTasks())
            {
                return Result::Pending;
            }
            if (load_task_->Get())
            {
                return Result::Fail;
            }

            sky_light_ = scene->GetSkyLight();
            if (sky_light_ == nullptr)
            {
                return Result::Fail;
            }
            sky_light_->SetSkyMap(MissingSkyMap);
            stage_ = Stage::WaitForMissingSky;
            return Result::Pending;

        case Stage::WaitForMissingSky: {
            if (scene->HasPendingAsyncTasks())
            {
                return Result::Pending;
            }

            const bool authored_path_preserved = sky_light_->GetSkyMapPath() == MissingSkyMap;
            const bool no_invalid_cube_map = !sky_light_->GetCubeMap();
            const bool async_failure_reported = !scene->DidAsyncTasksSucceed();
            return authored_path_preserved && no_invalid_cube_map && async_failure_reported ? Result::Pass
                                                                                            : Result::Fail;
        }

        default:
            return Result::Fail;
        }
    }

private:
    enum class Stage : uint8_t
    {
        InsertRenderBarrier,
        WaitForRenderBarrier,
        WaitForMissingScene,
        WaitForMissingSky,
    };

    static constexpr const char *MissingSkyMap = "missing/scene_load_failure.hdr";

    Stage stage_ = Stage::InsertRenderBarrier;

    std::shared_ptr<TaskFuture<>> render_barrier_;

    std::shared_ptr<TaskFuture<bool>> load_task_;

    SkyLight *sky_light_ = nullptr;
};

static TestCaseRegistrar<SceneLoadFailureTest> scene_load_failure_test_registrar("scene_load_failure");
} // namespace sparkle
