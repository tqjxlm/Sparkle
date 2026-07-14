#include "scene/Scene.h"

#include "core/Profiler.h"
#include "core/ThreadManager.h"
#include "core/task/TaskManager.h"
#include "renderer/proxy/MaterialRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "scene/SceneNode.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/light/SkyLight.h"
#include "scene/component/primitive/PrimitiveComponent.h"
#include "scene/material/Material.h"

#include <queue>
#include <ranges>

namespace sparkle
{
struct SceneAsyncTask::State
{
    std::atomic<int32_t> pending_tasks{0};
    std::atomic<bool> succeeded{true};
    std::atomic<bool> active{true};
};

SceneAsyncTask::SceneAsyncTask(std::shared_ptr<State> state) : state_(std::move(state))
{
}

SceneAsyncTask::~SceneAsyncTask()
{
    Complete(false);
}

void SceneAsyncTask::Complete(bool succeeded)
{
    if (completed_.exchange(true))
    {
        return;
    }

    if (!succeeded)
    {
        state_->succeeded.store(false);
    }

    const auto previous = state_->pending_tasks.fetch_sub(1);
    ASSERT(previous > 0);
}

bool SceneAsyncTask::IsActive() const
{
    return state_->active.load();
}

Scene::Scene()
    : render_proxy_(CreateRenderProxy()), root_node_(std::make_unique<SceneNode>(this, "SceneRoot")),
      async_state_(std::make_shared<SceneAsyncTask::State>())
{
}

Scene::~Scene()
{
    Cleanup();
}

void Scene::Tick()
{
    PROFILE_SCOPE("Scene::Tick");

    std::queue<SceneNode *> q;
    q.emplace(root_node_.get());

    while (!q.empty())
    {
        auto count = q.size();
        for (unsigned i = 0; i < count; i++)
        {
            SceneNode *node = q.front();
            q.pop();

            node->Tick();

            for (const auto &child : node->GetChildren())
            {
                q.emplace(child.get());
            }
        }
    }
}

void Scene::ProcessChange()
{
    PROFILE_SCOPE("Scene::ProcessChange");

    std::queue<SceneNode *> q;
    q.emplace(root_node_.get());

    while (!q.empty())
    {
        auto count = q.size();
        for (unsigned i = 0; i < count; i++)
        {
            SceneNode *node = q.front();
            q.pop();

            if (node->IsTransformDirty())
            {
                node->UpdateDirtyTransform();
            }

            for (const auto &child : node->GetChildren())
            {
                q.emplace(child.get());
            }
        }
    }
}

void Scene::SetMainCamera(const std::shared_ptr<CameraComponent> &main_camera)
{
    main_camera_ = main_camera.get();
}

void Scene::RecreateRenderProxy()
{
    ASSERT(ThreadManager::IsInRenderThread());

    Log(Info, "Recreating render proxy for the whole scene");

    root_node_->Traverse([](SceneNode *node) {
        for (const auto &component : node->GetComponents())
        {
            if (component->IsRenderable())
            {
                static_cast<RenderableComponent *>(component.get())->DestroyRenderProxy();
            }
        }
    });

    for (auto *material : material_usage_ | std::views::keys)
    {
        material->DestroyRenderProxy();
    }

    render_proxy_ = CreateRenderProxy();

    for (auto *material : material_usage_ | std::views::keys)
    {
        render_proxy_->AddMaterial(material->CreateRenderProxy());
    }

    root_node_->Traverse([](SceneNode *node) {
        for (const auto &component : node->GetComponents())
        {
            if (component->IsRenderable())
            {
                static_cast<RenderableComponent *>(component.get())->RecreateRenderProxy();
            }
        }
    });
}

std::unique_ptr<SceneRenderProxy> Scene::CreateRenderProxy()
{
    auto proxy = std::make_unique<SceneRenderProxy>();
    return proxy;
}

void Scene::RegisterPrimitive(PrimitiveComponent *primitive)
{
    ASSERT(!primitives_.contains(primitive));
    primitives_.insert(primitive);
}

void Scene::UnregisterPrimitive(PrimitiveComponent *primitive)
{
    ASSERT(primitives_.contains(primitive))

    primitives_.erase(primitive);
}

bool Scene::BoxCollides(const PrimitiveComponent *primitive) const
{
    return std::ranges::any_of(primitives_, [&primitive](const auto &existed) {
        return primitive != existed && primitive->GetWorldBoundingBox().Intersect(existed->GetWorldBoundingBox());
    });
}

std::shared_ptr<SceneAsyncTask> Scene::RegisterAsyncTask()
{
    if (async_state_->pending_tasks.fetch_add(1) == 0)
    {
        async_state_->succeeded.store(true);
    }
    return std::shared_ptr<SceneAsyncTask>(new SceneAsyncTask(async_state_));
}

bool Scene::HasPendingAsyncTasks() const
{
    return async_state_->pending_tasks.load() > 0;
}

bool Scene::DidAsyncTasksSucceed() const
{
    ASSERT(!HasPendingAsyncTasks());
    return async_state_->succeeded.load();
}

void Scene::Cleanup()
{
    async_state_->active.store(false);
    async_state_ = std::make_shared<SceneAsyncTask::State>();

    root_node_ = std::make_unique<SceneNode>(this, "SceneRoot");
    sky_light_ = nullptr;
    directional_light_ = nullptr;
    main_camera_ = nullptr;
}

void Scene::UnregisterMaterial(const std::shared_ptr<Material> &material)
{
    auto *material_ptr = material.get();

    if (!material_usage_.contains(material_ptr))
    {
        return;
    }

    material_usage_[material_ptr]--;

    if (material_usage_[material_ptr] == 0)
    {
        // we copy the shared_ptr in the lambda to keep the material living until proper cleanup
        TaskManager::RunInRenderThread([scene = GetRenderProxy(), material]() {
            scene->RemoveMaterial(material->GetRenderProxy());
            material->DestroyRenderProxy();
        });

        material_usage_.erase(material_ptr);
    }
}

void Scene::RegisterMaterial(Material *material)
{
    if (!material_usage_.contains(material))
    {
        TaskManager::RunInRenderThread(
            [scene = GetRenderProxy(), material]() { scene->AddMaterial(material->CreateRenderProxy()); });
    }

    material_usage_[material]++;
}

void Scene::SetSkyLight(SkyLight *light)
{
    sky_light_ = light;
}
} // namespace sparkle
