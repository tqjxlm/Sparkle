#include "scene/SceneManager.h"

#include "core/Logger.h"
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

namespace sparkle
{
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
            Vector3 color =
                Vector3(sampler::RandomUnit<true>(), sampler::RandomUnit<true>(), sampler::RandomUnit<true>());
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
    }
}

static std::shared_ptr<OrbitCameraComponent> CreateDefaultCamera()
{
    CameraComponent::Attribute camera_attribute;

    return std::make_shared<OrbitCameraComponent>(camera_attribute);
}

static std::shared_ptr<TaskFuture<>> LoadSceneFromFile(Scene *scene, const std::string &file_path)
{
    // always load a default camera as the fallback behaviour
    auto main_camera = CreateDefaultCamera();
    auto camera_node = std::make_shared<SceneNode>(scene, "DefaultCamera");
    camera_node->AddComponent(main_camera);
    scene->GetRootNode()->AddChild(camera_node);
    main_camera->Setup(Zeros, 10.f, 0.f, 0.f);
    scene->SetMainCamera(main_camera);

    return SceneDataFactory::Load(file_path, scene)->Then([scene](const std::shared_ptr<SceneNode> &node) {
        scene->GetRootNode()->AddChild(node);
    });
}

static std::shared_ptr<TaskFuture<>> LoadTestScene(Scene *scene)
{
    Log(Info, "Loading standard scene");

    auto *scene_root = scene->GetRootNode();

    // camera
    auto main_camera = CreateDefaultCamera();
    auto camera_node = std::make_shared<SceneNode>(scene, "DefaultCamera");
    camera_node->AddComponent(main_camera);
    scene_root->AddChild(camera_node);

    scene->SetMainCamera(main_camera);
    main_camera->Setup(Zeros, 25.f, 10.f, -20.f);

    // lights
    SceneManager::AddDefaultSky(scene);
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

    std::vector<std::shared_ptr<TaskFuture<>>> async_tasks;

    // models
    auto water_bottle_task =
        SceneDataFactory::Load("models/WaterBottle/WaterBottle.gltf", scene)
            ->Then([scene_root](const std::shared_ptr<SceneNode> &node) {
                node->SetTransform({-4.f, -4.f, 2.7f}, {0, 0, utilities::ToRadian(-30.f)}, Ones * 2.f);
                scene_root->AddChild(node);
            });
    async_tasks.emplace_back(water_bottle_task);

    auto boom_box_task = SceneDataFactory::Load("models/BoomBox/BoomBox.gltf", scene)
                             ->Then([scene_root](const std::shared_ptr<SceneNode> &node) {
                                 node->SetTransform({5.f, 4.f, 3.f}, {0, 0, utilities::ToRadian(30.f)}, Ones * 3.f);
                                 scene_root->AddChild(node);
                             });
    async_tasks.emplace_back(boom_box_task);

    return TaskManager::OnAll(async_tasks)->Then([scene]() {
        // a lot of random spheres
        SceneManager::GenerateRandomSpheres(*scene, 10);
    });
}

std::shared_ptr<TaskFuture<>> SceneManager::LoadScene(Scene *scene, const std::string &scene_name)
{
    if (scene_name.empty() || scene_name == "Test")
    {
        return LoadTestScene(scene);
    }

    return LoadSceneFromFile(scene, scene_name);
}

void SceneManager::RemoveLastNode(Scene *scene)
{
    auto *root_node = scene->GetRootNode();
    const auto &last_child = root_node->GetChildren().back();
    root_node->RemoveChild(last_child);
}

void SceneManager::AddDefaultSky(Scene *scene)
{
    auto [sky_light_node, sky_light] = MakeNodeWithComponent<SkyLight>(scene, scene->GetRootNode(), "DefaultSky");
    sky_light->SetSkyMap("skymap/studio_garden.hdr");
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
} // namespace sparkle
