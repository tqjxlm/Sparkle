#include "scene/component/RenderableComponent.h"

#include "core/ThreadManager.h"
#include "core/task/TaskManager.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "scene/Scene.h"


namespace sparkle
{
RenderableComponent::RenderableComponent()
{
    is_renderable_ = true;
}

RenderableComponent::~RenderableComponent()
{
    if (render_proxy_)
    {
        auto *scene_proxy = node_->GetScene()->GetRenderProxy();
        ASSERT(scene_proxy);

        TaskManager::RunInRenderThread(
            [scene_proxy, render_proxy = render_proxy_]() { scene_proxy->RemoveRenderProxy(render_proxy); });
    }
}

void RenderableComponent::DestroyRenderProxy()
{
    ASSERT(ThreadManager::IsInRenderThread());

    if (!render_proxy_)
    {
        return;
    }

    auto *scene_proxy = node_->GetScene()->GetRenderProxy();
    ASSERT(scene_proxy);

    if (scene_proxy)
    {
        scene_proxy->RemoveRenderProxy(render_proxy_);
    }

    render_proxy_ = nullptr;
}

void RenderableComponent::RecreateRenderProxy()
{
    ASSERT(ThreadManager::IsInRenderThread());

    auto *scene_proxy = node_->GetScene()->GetRenderProxy();
    ASSERT(scene_proxy);

    DestroyRenderProxy();

    render_proxy_ = scene_proxy->AddRenderProxy(CreateRenderProxy());

    render_proxy_->UpdateTransform(GetTransform());
}

void RenderableComponent::OnTransformChange()
{
    Component::OnTransformChange();

    TaskManager::RunInRenderThread([this]() {
        if (!render_proxy_)
        {
            return;
        }

        render_proxy_->UpdateTransform(GetTransform());
    });
}
} // namespace sparkle
