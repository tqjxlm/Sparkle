#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "application/TestCase.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "core/math/Utilities.h"
#include "core/task/TaskFuture.h"
#include "core/task/TaskManager.h"
#include "io/Mesh.h"
#include "io/scene/SceneDataFactory.h"
#include "io/scene/USDExporter.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/component/camera/OrbitCameraComponent.h"
#include "scene/component/primitive/MeshPrimitive.h"
#include "scene/component/primitive/SpherePrimitive.h"
#include "scene/material/DieletricMaterial.h"
#include "scene/material/LambertianMaterial.h"
#include "scene/material/MaterialManager.h"

namespace sparkle
{
// covers every exportable component. resources/packed/TestScene.usda is the export of this
// scene; regenerate it by rerunning this test (see docs/USD.md).
static std::shared_ptr<TaskFuture<>> BuildTestScene(Scene *scene)
{
    Log(Info, "Building procedural test scene");

    scene->Cleanup();

    auto *scene_root = scene->GetRootNode();
    scene_root->SetName("TestScene");

    CameraComponent::Attribute camera_attribute;
    auto main_camera = std::make_shared<OrbitCameraComponent>(camera_attribute);
    auto camera_node = std::make_shared<SceneNode>(scene, "DefaultCamera");
    camera_node->AddComponent(main_camera);
    main_camera->Setup(Zeros, 25.f, 10.f, -20.f);

    scene->SetMainCamera(main_camera);
    scene_root->AddChild(camera_node);

    SceneManager::AddDefaultDirectionalLight(scene);

    auto &material_manager = MaterialManager::Instance();

    auto white_marble_material = material_manager.GetOrCreateMaterial<LambertianMaterial>({.name = "WhiteMarble"});
    auto glass_material = material_manager.GetOrCreateMaterial<DieletricMaterial>({.eta = 1.6f, .name = "Glass"});

    {
        auto [node, primitive] = MakeNodeWithComponent<MeshPrimitive>(scene, scene_root, "floor", Mesh::GetUnitCube());
        node->SetTransform(Up * -0.001f, Zeros, {50.f, 50.f, 0.001f});
        primitive->SetMaterial(white_marble_material);
    }

    {
        auto [node, primitive] = MakeNodeWithComponent<SpherePrimitive>(scene, scene_root, "glass sphere");
        node->SetTransform({0.f, 0.f, 2.f}, Zeros, Ones * 2.f);
        primitive->SetMaterial(glass_material);
    }
    {
        auto [node, primitive] = MakeNodeWithComponent<SpherePrimitive>(scene, scene_root, "lambert sphere");
        node->SetTransform({-4.f, 4.f, 2.f}, Zeros, Ones * 2.f);
        primitive->SetMaterial(white_marble_material);
    }
    {
        auto [node, primitive] = MakeNodeWithComponent<SpherePrimitive>(scene, scene_root, "gold sphere");
        node->SetTransform({4.f, -4.f, 2.f}, Zeros, Ones * 2.f);
        primitive->SetMaterial(material_manager.GetMetalMaterials()[MaterialManager::GOLD]);
    }

    std::vector<std::shared_ptr<TaskFuture<>>> model_tasks;

    auto water_bottle_task =
        SceneDataFactory::Load(Path::Resource("models/WaterBottle/WaterBottle.gltf"), scene)
            ->Then([scene_root](const std::shared_ptr<SceneNode> &node) {
                if (node)
                {
                    node->SetTransform({-4.f, -4.f, 2.7f}, {0, 0, utilities::ToRadian(-30.f)}, Ones * 2.f);
                    scene_root->AddChild(node);
                }
            });
    model_tasks.emplace_back(water_bottle_task);

    auto boom_box_task = SceneDataFactory::Load(Path::Resource("models/BoomBox/BoomBox.gltf"), scene)
                             ->Then([scene_root](const std::shared_ptr<SceneNode> &node) {
                                 if (node)
                                 {
                                     node->SetTransform({5.f, 4.f, 3.f}, {0, 0, utilities::ToRadian(30.f)}, Ones * 3.f);
                                     scene_root->AddChild(node);
                                 }
                             });
    model_tasks.emplace_back(boom_box_task);

    auto models_loaded = TaskManager::OnAll(model_tasks);
    auto last_task_finished = models_loaded;

    // one task per sphere so each collision check sees the previously placed ones
    for (int i = 0; i < 10; i++)
    {
        last_task_finished = last_task_finished->Then([scene]() { SceneManager::GenerateRandomSpheres(*scene, 1); });
    }

    return last_task_finished->Then([scene]() { SceneManager::AddDefaultSky(scene)->Forget(); });
}

// exports the active scene to USD, loads the exported file back as the active scene, and takes a
// screenshot of both. tests/usd/usd_roundtrip_test.py drives this test and compares the screenshots.
// without an explicit --scene, BuildTestScene() provides the scene to round-trip.
class UsdRoundTripTest : public TestCase
{
public:
    static constexpr const char *ExportedScenePath = "usd_export/scene.usda";

    // give a freshly (re)built scene some frames to settle (proxy rebuilds, accumulation restart)
    // before trusting the renderer's screenshot readiness again.
    static constexpr uint32_t SceneSettleFrames = 60;

    Result OnTick(AppFramework &app) override
    {
        auto *render_framework = app.GetRenderFramework();

        switch (stage_)
        {
        case Stage::Start: {
            auto *scene_config = ConfigManager::Instance().GetConfig<std::string>("scene");
            if (scene_config != nullptr && !scene_config->Get().empty())
            {
                stage_ = Stage::WaitOriginalRender;
                break;
            }

            // only tear down the app-loaded scene once the renderer has fully synced it:
            // component attach tasks hold raw component pointers on the render thread, so an
            // early Cleanup() frees them mid-flight.
            if (frame_ >= SceneSettleFrames && render_framework->IsReadyForAutoScreenshot())
            {
                scene_task_succeeded_ = true;
                scene_task_ = BuildTestScene(app.GetScene());
                next_stage_ = Stage::WaitOriginalRender;
                stage_ = Stage::WaitSceneTasks;
            }
            break;
        }

        case Stage::WaitSceneTasks:
            if (scene_task_->IsReady() && !app.GetScene()->HasPendingAsyncTasks())
            {
                scene_task_.reset();
                if (!scene_task_succeeded_)
                {
                    Log(Error, "UsdRoundTripTest: scene load failed");
                    return Result::Fail;
                }
                settle_frame_ = frame_ + SceneSettleFrames;
                stage_ = next_stage_;
            }
            break;

        case Stage::WaitOriginalRender:
            if (frame_ >= settle_frame_ && render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("usd_round_trip_original");
                stage_ = Stage::WaitOriginalScreenshot;
            }
            break;

        case Stage::WaitOriginalScreenshot:
            if (screenshot_->IsCompleted())
            {
                if (!USDExporter::Export(app.GetScene(), Path::Internal(ExportedScenePath)))
                {
                    Log(Error, "UsdRoundTripTest: export failed");
                    return Result::Fail;
                }

                // the exported file carries its own sky, lights and camera. no defaults wanted.
                scene_task_ = SceneManager::LoadScene(app.GetScene(), Path::Internal(ExportedScenePath), false, false)
                                  ->Then([this](bool succeeded) { scene_task_succeeded_ = succeeded; });
                next_stage_ = Stage::WaitReimportedRender;
                stage_ = Stage::WaitSceneTasks;
            }
            break;

        case Stage::WaitReimportedRender:
            if (frame_ >= settle_frame_ && render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("usd_round_trip_reimported");
                stage_ = Stage::WaitReimportedScreenshot;
            }
            break;

        case Stage::WaitReimportedScreenshot:
            if (screenshot_->IsCompleted())
            {
                return Result::Pass;
            }
            break;

        default:
            break;
        }

        return Result::Pending;
    }

private:
    enum class Stage : uint8_t
    {
        Start,
        WaitSceneTasks,
        WaitOriginalRender,
        WaitOriginalScreenshot,
        WaitReimportedRender,
        WaitReimportedScreenshot,
    };

    Stage stage_ = Stage::Start;
    Stage next_stage_ = Stage::Start;

    std::shared_ptr<ScreenshotRequest> screenshot_;
    std::shared_ptr<TaskFuture<void>> scene_task_;

    bool scene_task_succeeded_ = true;

    uint32_t settle_frame_ = 0;
};

static TestCaseRegistrar<UsdRoundTripTest> usd_round_trip_test_registrar("usd_round_trip");
} // namespace sparkle
