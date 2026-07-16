#include "scene/SceneManager.h"

#include "core/ConfigManager.h"
#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/Profiler.h"
#include "core/math/Sampler.h"
#include "core/math/Utilities.h"
#include "core/task/TaskManager.h"
#include "io/scene/SceneDataFactory.h"
#include "scene/Scene.h"
#include "scene/component/camera/OrbitCameraComponent.h"
#include "scene/component/light/DirectionalLight.h"
#include "scene/component/light/SkyLight.h"
#include "scene/component/primitive/SpherePrimitive.h"
#include "scene/material/DieletricMaterial.h"
#include "scene/material/LambertianMaterial.h"
#include "scene/material/MaterialManager.h"

#include <imgui.h>

namespace sparkle
{
static constexpr const char *DefaultSkyMapFile = "skymap/studio_garden.hdr";
// must live at the resource root: its asset references (models/..., skymap/...) are relative to
// the file and tinyusdz rejects ".." in asset paths
static constexpr const char *TestSceneFile = "TestScene.usda";
static std::vector<SceneNode *> debug_spheres;

static void UpdateSceneConfig(const Path &asset_path)
{
    auto *scene_config = ConfigManager::Instance().GetConfig<std::string>("scene");
    if (!scene_config)
    {
        return;
    }

    std::string new_value;
    if (asset_path.IsValid() && !asset_path.path.empty())
    {
        new_value = asset_path.path.string();
    }

    if (scene_config->Get() != new_value)
    {
        scene_config->Set(new_value);
    }
}

void SceneManager::GenerateRandomSpheres(Scene &scene, unsigned count)
{
    auto &material_manager = MaterialManager::Instance();

    const float min_radius = 0.4f;
    const float max_radius = 1.f;

    const Vector3 spread_center = Zeros;
    const float spread_radius = 13.f;

    const float lambertian_ratio = 0.7f;
    const float dieletric_ratio = 0.1f;

    for (auto i = 0u; i < count; i++)
    {
        auto radius = utilities::Lerp(min_radius, max_radius, sampler::RandomUnit<true>());
        auto [node, primitive] =
            MakeNodeWithComponent<SpherePrimitive>(&scene, scene.GetRootNode(), "random sphere " + std::to_string(i));

        // randomly pick a material from built-in materials
        const float material_type = sampler::RandomUnit<true>();

        std::shared_ptr<Material> material;
        if (material_type < lambertian_ratio)
        {
            auto r = sampler::RandomUnit<true>();
            auto g = sampler::RandomUnit<true>();
            auto b = sampler::RandomUnit<true>();
            Vector3 color = Vector3(r, g, b);
            material =
                material_manager.CreateMaterial<LambertianMaterial>({.base_color = color, .name = "RandomLambertian"});
        }
        else if (material_type < lambertian_ratio + dieletric_ratio)
        {
            auto eta = sampler::RandomUnit<true>() * 2.f + 1.3f;
            material = material_manager.CreateMaterial<DieletricMaterial>(
                {.base_color = Ones, .eta = eta, .name = "RandomDieletric"});
        }
        else
        {
            material = material_manager.GetRandomMetalMaterial();
        }
        primitive->SetMaterial(material);

        // find a suitable random position
        do
        {
            Vector2 position_xy = sampler::UnitDisk<true>() * spread_radius;
            Vector3 position = spread_center + Vector3(position_xy.x(), position_xy.y(), radius);
            node->SetTransform(position, Zeros, Ones * radius);
        } while (scene.BoxCollides(primitive.get()));

        debug_spheres.push_back(node.get());
    }
}

static std::shared_ptr<OrbitCameraComponent> CreateDefaultCamera()
{
    CameraComponent::Attribute camera_attribute;

    return std::make_shared<OrbitCameraComponent>(camera_attribute);
}

static std::shared_ptr<TaskFuture<bool>> LoadSceneFromFile(Scene *scene, const Path &path)
{
    return SceneDataFactory::Load(path, scene)->Then([scene, path](const std::shared_ptr<SceneNode> &node) {
        if (node)
        {
            scene->GetRootNode()->AddChild(node);
            return true;
        }

        Log(Error, "failed to load model {}", path.path.string());
        return false;
    });
}

std::shared_ptr<TaskFuture<bool>> SceneManager::LoadScene(Scene *scene, const Path &asset_path, bool need_default_sky,
                                                          bool need_default_lighting)
{
    PROFILE_SCOPE_LOG("SceneManager::LoadScene");

    // TODO(tqjxlm): handle pending async tasks

    Log(Info, "Loading scene... {}", asset_path.path.string());

    UpdateSceneConfig(asset_path);

    scene->Cleanup();

    debug_spheres.clear();

    // always load a default camera as the fallback behaviour
    auto main_camera = CreateDefaultCamera();
    auto camera_node = std::make_shared<SceneNode>(scene, "DefaultCamera");
    camera_node->AddComponent(main_camera);
    main_camera->Setup(Zeros, 10.f, 0.f, 0.f);

    scene->SetMainCamera(main_camera);
    scene->GetRootNode()->AddChild(camera_node);

    std::shared_ptr<TaskFuture<bool>> load_task;
    if (!asset_path.IsValid() || asset_path.path.empty())
    {
        // No scene path given (the `scene` cvar defaults to empty) => load the packaged TestScene.
        // This is the default scene and is exactly what the CI ground-truth images are rendered from.
        // It carries its own sky, lights and camera. See tests/usd/UsdRoundTripTest.cpp for how to
        // regenerate it.
        load_task = LoadSceneFromFile(scene, Path::Resource(TestSceneFile));
        scene->GetRootNode()->SetName("TestScene");
    }
    else
    {
        // A non-empty `scene` is a model/scene file PATH (e.g. "models/foo.gltf"), NOT a scene name:
        // "TestScene" here would be treated as a file and fail to load.
        load_task = LoadSceneFromFile(scene, asset_path);
        scene->GetRootNode()->SetName(asset_path.path.parent_path().string());
    }

    return load_task->Then([scene, need_default_sky, need_default_lighting](bool succeeded) {
        if (need_default_lighting && !scene->GetDirectionalLight())
        {
            SceneManager::AddDefaultDirectionalLight(scene);
        }

        if (need_default_sky && !scene->GetSkyLight())
        {
            SceneManager::AddDefaultSky(scene)->Forget();
        }

        return succeeded;
    });
}

void SceneManager::RemoveLastDebugSphere(Scene *scene)
{
    auto *root_node = scene->GetRootNode();
    auto *last_sphere = debug_spheres.back();
    root_node->RemoveChild(last_sphere);
    debug_spheres.pop_back();
}

std::shared_ptr<TaskFuture<void>> SceneManager::AddDefaultSky(Scene *scene)
{
    auto [sky_light_node, sky_light] = MakeNodeWithComponent<SkyLight>(scene, nullptr, "DefaultSky");
    auto scene_task = scene->RegisterAsyncTask();
    return TaskManager::RunInWorkerThread([sky_light]() { sky_light->SetSkyMap(DefaultSkyMapFile); })
        ->Then([scene, sky_light_node, scene_task]() {
            if (scene_task->IsActive())
            {
                scene->GetRootNode()->AddChild(sky_light_node);
            }
            scene_task->Complete(true);
        });
}

void SceneManager::AddDefaultDirectionalLight(Scene *scene)
{
    auto *scene_root = scene->GetRootNode();

    auto [dir_light_node, dir_light] =
        MakeNodeWithComponent<DirectionalLight>(scene, scene_root, "DefaultDirectionalLight");

    dir_light->SetColor(Ones * 0.3f);
    dir_light_node->SetTransform(Zeros, utilities::ToRadian(Vector3(45.7995f, -16.5189f, -37.9306f)));

    if (auto *sky_light = scene->GetSkyLight())
    {
        if (sky_light->HasSkyMap())
        {
            // the sun is extracted by the async sky cook. weak captures guard against the
            // scene being cleaned up before delivery; the sky is re-looked-up through the
            // scene because it lives in the same generation as the captured light.
            sky_light->OnCooked()->Then(
                [scene, weak_node = std::weak_ptr<SceneNode>(dir_light_node),
                 weak_light = std::weak_ptr<DirectionalLight>(dir_light)]() {
                    auto node = weak_node.lock();
                    auto light = weak_light.lock();
                    auto *sky = scene->GetSkyLight();
                    if (!node || !light || (sky == nullptr) || !sky->GetCubeMap())
                    {
                        return;
                    }

                    light->SetColor(sky->GetSunBrightness());
                    node->SetTransform(Zeros, sky->GetSunDirection());
                },
                TargetThread::Main);
        }
    }
}

void SceneManager::DrawUi(Scene *scene, bool need_default_sky, bool need_default_lighting)
{
    auto *file_manager = FileManager::GetNativeFileManager();

    ImGui::Text("Available Models:");
    ImGui::Separator();

    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 30);

    {
        bool is_current_scene = scene->GetRootNode()->GetName() == "TestScene";

        if (ImGui::Selectable("TestScene", is_current_scene))
        {
            SceneManager::LoadScene(scene, {}, need_default_sky, need_default_lighting)->Forget();
        }
    }

    static auto cached_model_dirs = file_manager->ListDirectory(Path::Resource("models"));
    for (const auto &entry : cached_model_dirs)
    {
        bool is_current_scene = scene->GetRootNode()->GetName() == entry.path;

        if (ImGui::Selectable(entry.path.filename().string().c_str(), is_current_scene))
        {
            auto files = file_manager->ListDirectory(entry);

            for (const auto &file : files)
            {
                auto ext = file.path.extension().string();
                if (ext == ".gltf" || ext == ".glb" || ext == ".usda" || ext == ".usdc" || ext == ".usdz")
                {
                    SceneManager::LoadScene(scene, file, need_default_sky, need_default_lighting)->Forget();
                    continue;
                }
            }

            Log(Warn, "No supported model file found in: {}", entry.path.string());
        }
    }

    ImGui::PopStyleVar(1);
}
} // namespace sparkle
