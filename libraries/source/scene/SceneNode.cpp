#include "scene/SceneNode.h"

#include "core/Container.h"
#include "core/Logger.h"
#include "core/ThreadManager.h"
#include "scene/Scene.h"
#include "scene/component/Component.h"

namespace sparkle
{
void SceneNode::AddComponent(const std::shared_ptr<Component> &component)
{
    ASSERT(!component->GetNode());

    components_.push_back(component);
    component->SetNode(this);
}

void SceneNode::Tick()
{
    for (auto &component : components_)
    {
        if (component->ShouldTick())
        {
            component->Tick();
        }
    }
}

void SceneNode::UpdateDirtyTransform()
{
    transform_dirty_ = false;

    if (parent_)
    {
        const auto &parent_transform = parent_->GetTransform();
        world_transform_ = Transform(parent_transform.GetTransformData() * local_transform_.GetTransformData(),
                                     local_transform_.GetInvTransformData() * parent_transform.GetInvTransformData());
    }
    else
    {
        world_transform_ = local_transform_;
    }

    for (auto &component : components_)
    {
        component->OnTransformChange();
    }
}

void SceneNode::MarkTransformDirty()
{
    transform_dirty_ = true;
}

void SceneNode::AddChild(const std::shared_ptr<SceneNode> &child)
{
    ASSERT(child);
    ASSERT_F(!IsInScene() || ThreadManager::IsInMainThread(), "Scene management should only happen in main thread");

    if (child->sibling_index_ != UINT_MAX)
    {
        Log(Error, "this node already has a child. remove it from its parent before re-parenting");
        return;
    }

    child->sibling_index_ = static_cast<uint32_t>(children_.size());
    child->parent_ = this;
    children_.push_back(child);

    if (IsInScene())
    {
        child->OnAttachToScene();
    }
}

void SceneNode::RemoveChild(SceneNode *child)
{
    ASSERT_F(ThreadManager::IsInMainThread(), "Scene management should only happen in main thread");

    auto index_to_remove = child->sibling_index_;
    child->sibling_index_ = UINT_MAX;
    child->parent_ = nullptr;

    if (RemoveAtSwap(children_, index_to_remove))
    {
        children_[index_to_remove]->sibling_index_ = index_to_remove;
    }
}

bool SceneNode::IsInScene() const
{
    return GetRootNode() == scene_->GetRootNode();
}

void SceneNode::OnAttachToScene()
{
    ASSERT(IsInScene());

    for (auto &component : components_)
    {
        component->OnAttach();
    }

    for (auto &child : children_)
    {
        child->OnAttachToScene();
    }
}
} // namespace sparkle
