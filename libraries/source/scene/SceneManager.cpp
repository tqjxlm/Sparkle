#include "scene/SceneManager.h"

#include "core/ConfigManager.h"
#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/Profiler.h"
#include "core/math/Sampler.h"
#include "core/math/Utilities.h"
#include "core/task/TaskManager.h"
#include "io/Mesh.h"
#include "io/scene/SceneDataFactory.h"
#include "scene/Scene.h"
#include "scene/component/camera/OrbitCameraComponent.h"
#include "scene/component/light/DirectionalLight.h"
#include "scene/component/light/SkyLight.h"
#include "scene/component/primitive/MeshPrimitive.h"
#include "scene/component/primitive/SpherePrimitive.h"
#include "scene/material/DieletricMaterial.h"
#include "scene/material/LambertianMaterial.h"
#include "scene/material/MaterialManager.h"

#include <imgui.h>

namespace sparkle
{
static constexpr const char *DefaultSkyMapFile = "skymap/studio_garden.hdr";
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
            material = material_manager.GetOrCreateMaterial<LambertianMaterial>(
                {.base_color = color, .name = "RandomLambertian"});
        }
        else if (material_type < lambertian_ratio + dieletric_ratio)
        {
            auto eta = sampler::RandomUnit<true>() * 2.f + 1.3f;
            material = material_manager.GetOrCreateMaterial<DieletricMaterial>(
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

static std::shared_ptr<TaskFuture<>> LoadSceneFromFile(Scene *scene, const Path &path)
{
    return SceneDataFactory::Load(path, scene)->Then([scene, path](const std::shared_ptr<SceneNode> &node) {
        if (node)
        {
            scene->GetRootNode()->AddChild(node);
        }
        else
        {
            Log(Error, "failed to load model {}", path.path.string());
        }
    });
}

static std::shared_ptr<TaskFuture<>> LoadTestScene(Scene *scene)
{
    Log(Info, "Loading standard scene");

    auto *scene_root = scene->GetRootNode();

    auto *main_camera = static_cast<OrbitCameraComponent *>(scene->GetMainCamera());
    main_camera->Setup(Zeros, 25.f, 10.f, -20.f);

    SceneManager::AddDefaultDirectionalLight(scene);

    auto &material_manager = MaterialManager::Instance();

    auto white_marble_material = material_manager.GetOrCreateMaterial<LambertianMaterial>({.name = "WhiteMarble"});
    auto glass_material = material_manager.GetOrCreateMaterial<DieletricMaterial>({.eta = 1.6f, .name = "Glass"});

    // basic primitives
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

    // models
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

    // append a bunch of random spheres to the task chain
    for (int i = 0; i < 10; i++)
    {
        last_task_finished = last_task_finished->Then([scene]() { SceneManager::GenerateRandomSpheres(*scene, 1); });
    }

    return last_task_finished;
}

std::shared_ptr<TaskFuture<void>> SceneManager::LoadScene(Scene *scene, const Path &asset_path, bool need_default_sky,
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

    std::shared_ptr<TaskFuture<>> load_task;
    if (!asset_path.IsValid() || asset_path.path.empty())
    {
        load_task = LoadTestScene(scene);
        need_default_sky = true;
        scene->GetRootNode()->SetName("TestScene");
    }
    else
    {
        load_task = LoadSceneFromFile(scene, asset_path);
        scene->GetRootNode()->SetName(asset_path.path.parent_path().string());
    }

    return load_task->Then([scene, need_default_sky, need_default_lighting]() {
        if (need_default_lighting && !scene->GetDirectionalLight())
        {
            SceneManager::AddDefaultDirectionalLight(scene);
        }

        if (need_default_sky && !scene->GetSkyLight())
        {
            SceneManager::AddDefaultSky(scene)->Forget();
        }
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
    scene->RegisterAsyncTask();
    return TaskManager::RunInWorkerThread([sky_light]() { sky_light->SetSkyMap(DefaultSkyMapFile); })
        ->Then([scene, sky_light_node]() {
            scene->GetRootNode()->AddChild(sky_light_node);
            scene->UnregisterAsyncTask();
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
        if (sky_light->GetSkyMap())
        {
            dir_light->SetColor(sky_light->GetSunBrightness());
            dir_light_node->SetTransform(Zeros, sky_light->GetSunDirection());
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
