#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/RenderProxy.h"
#include "core/task/TaskManager.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/light/LightSource.h"
#include "scene/component/primitive/SpherePrimitive.h"
#include "scene/material/LambertianMaterial.h"
#include "scene/material/MaterialManager.h"

#include <atomic>
#include <memory>

namespace sparkle
{
namespace
{
struct RenderProxyCount
{
    std::atomic<uint32_t> created{0};
    std::atomic<uint32_t> destroyed{0};
};

class ProbeRenderProxy : public RenderProxy
{
public:
    explicit ProbeRenderProxy(RenderProxyCount &count) : count_(count)
    {
        count_.created.fetch_add(1);
    }

    ~ProbeRenderProxy() override
    {
        count_.destroyed.fetch_add(1);
    }

private:
    RenderProxyCount &count_;
};

// a renderable component that counts the render proxies the render thread creates for it and destroys
class RenderProbe : public LightSourceComponent
{
public:
    explicit RenderProbe(RenderProxyCount &count) : count_(count)
    {
    }

private:
    std::unique_ptr<RenderProxy> CreateRenderProxy() override
    {
        return std::make_unique<ProbeRenderProxy>(count_);
    }

    RenderProxyCount &count_;
};
} // namespace

// reloads the scene twice in one frame, with a component attached in between: the second load replaces the scene
// while the first still runs and while render commands of the replaced components are queued. it then destroys a
// scene of its own that the render thread knows. the scene and the render thread must hold what the last load
// brought and nothing else, and the frame must match the ground truth of a scene loaded once.
class SceneReplacementTest : public TestCase
{
    // a replaced scene needs some frames before the renderer's screenshot readiness speaks for it
    static constexpr uint32_t SettleFrames = 60;

public:
    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        auto *scene = app.GetScene();
        auto *framework = app.GetRenderFramework();

        switch (phase_)
        {
        case Phase::Replace:
            Replace(app);
            phase_ = Phase::WaitLoaded;
            return Result::Pending;

        case Phase::WaitLoaded:
            if (!superseded_load_->IsReady() || !load_->IsReady() || scene->HasPendingAsyncTasks())
            {
                return Result::Pending;
            }
            VerifyReplacement(scene);
            phase_ = Phase::BuildOwnScene;
            return Result::Pending;

        case Phase::BuildOwnScene:
            BuildOwnScene();
            // the next phase starts once the render thread ran the commands of the scene just built
            RunOnRenderThread([] {});
            phase_ = Phase::DestroyOwnScene;
            return Result::Pending;

        case Phase::DestroyOwnScene:
            DestroyOwnScene();
            phase_ = Phase::WaitRendered;
            wait_until_frame_ = frame_ + SettleFrames;
            return Result::Pending;

        case Phase::WaitRendered:
            if (frame_ < wait_until_frame_ || !framework->IsReadyForAutoScreenshot())
            {
                return Result::Pending;
            }
            screenshot_ = framework->RequestTakeScreenshot("screenshot");
            phase_ = Phase::Screenshot;
            return Result::Pending;

        case Phase::Screenshot:
            if (!screenshot_->IsCompleted())
            {
                return Result::Pending;
            }
            Expect(probe_proxies_.destroyed.load() == probe_proxies_.created.load(),
                   "the render thread destroyed every render proxy of the replaced scene");
            return HasFailed() ? Result::Fail : Result::Pass;

        default:
            return Result::Fail;
        }
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 3000;
    }

private:
    enum class Phase : uint8_t
    {
        Replace,
        WaitLoaded,
        BuildOwnScene,
        DestroyOwnScene,
        WaitRendered,
        Screenshot,
    };

    void Replace(AppFramework &app)
    {
        auto *scene = app.GetScene();
        const auto &app_config = app.GetAppConfig();
        const auto scene_path = Path::Resource(app_config.scene);
        const bool default_lighting = app.GetRenderConfig().IsRaterizationMode();

        primitive_count_ = scene->GetPrimitives().size();

        superseded_load_ = SceneManager::LoadScene(scene, scene_path, app_config.default_skybox, default_lighting);

        auto probe_node = std::make_shared<SceneNode>(scene, "RenderProbe");
        probe_node->AddComponent(std::make_shared<RenderProbe>(probe_proxies_));
        scene->GetRootNode()->AddChild(probe_node);

        load_ = SceneManager::LoadScene(scene, scene_path, app_config.default_skybox, default_lighting);

        Expect(probe_proxies_.created.load() == 1,
               "the render commands queued for a replaced component run before it is destroyed");
    }

    void VerifyReplacement(Scene *scene)
    {
        Expect(!superseded_load_->Get(), "a load the scene was replaced under reports failure");
        Expect(load_->Get(), "the replacing load succeeds");
        Expect(scene->GetPrimitives().size() == primitive_count_, "the scene holds the primitives of one load");

        const auto *main_camera = scene->GetMainCamera();
        bool main_camera_attached = false;
        scene->GetRootNode()->Traverse([main_camera, &main_camera_attached](SceneNode *node) {
            for (const auto &component : node->GetComponents())
            {
                main_camera_attached |= component.get() == main_camera;
            }
        });
        Expect(main_camera_attached, "the main camera is attached to the scene");

        RunOnRenderThread([this, scene_proxy = scene->GetRenderProxy(), primitive_count = primitive_count_,
                           has_sky = scene->GetSkyLight() != nullptr] {
            Expect(scene_proxy->GetPrimitives().size() == primitive_count,
                   "the render thread holds the primitives of one load");
            Expect(scene_proxy->GetCamera() != nullptr, "the render thread holds a camera");
            Expect((scene_proxy->GetSkyLight() != nullptr) == has_sky, "the render thread holds the sky of the scene");
        });
    }

    void BuildOwnScene()
    {
        own_scene_ = std::make_unique<Scene>();
        own_material_ = MaterialManager::Instance().CreateMaterial<LambertianMaterial>({.name = "OwnSceneMaterial"});

        auto sphere = std::make_shared<SpherePrimitive>();
        sphere->SetMaterial(own_material_);

        auto sphere_node = std::make_shared<SceneNode>(own_scene_.get(), "OwnSceneSphere");
        sphere_node->AddComponent(sphere);
        own_scene_->GetRootNode()->AddChild(sphere_node);
    }

    // the command that removes a material from its scene render proxy is the one that clears the material's proxy
    void DestroyOwnScene()
    {
        Expect(own_material_->GetRenderProxy() != nullptr, "the render thread knows the material of the built scene");

        own_scene_.reset();

        Expect(own_material_->GetRenderProxy() == nullptr,
               "the removal commands of a destroyed scene run before its render proxy is destroyed");
    }

    template <typename Task> void RunOnRenderThread(Task &&task)
    {
        task_pending_.store(true, std::memory_order_release);
        TaskManager::RunInRenderThread([this, render_task = std::forward<Task>(task)] {
            render_task();
            task_pending_.store(false, std::memory_order_release);
        });
    }

    Phase phase_ = Phase::Replace;
    uint32_t wait_until_frame_ = 0;

    std::shared_ptr<TaskFuture<bool>> superseded_load_;
    std::shared_ptr<TaskFuture<bool>> load_;
    size_t primitive_count_ = 0;
    RenderProxyCount probe_proxies_;

    std::unique_ptr<Scene> own_scene_;
    std::shared_ptr<Material> own_material_;

    std::shared_ptr<ScreenshotRequest> screenshot_;

    std::atomic<bool> task_pending_{false};
};

static TestCaseRegistrar<SceneReplacementTest> scene_replacement_test_registrar("scene_replacement");
} // namespace sparkle
