#pragma once

#include "core/math/AABB.h"
#include "scene/SceneNode.h"

namespace sparkle
{
class Component
{
public:
    virtual ~Component() = default;

    virtual void OnTransformChange()
    {
    }

    virtual void Tick();

    virtual void OnAttach()
    {
        ASSERT(!is_attached_);
        is_attached_ = true;
    }

    [[nodiscard]] virtual AABB GetWorldBoundingBox() const
    {
        return {};
    }

    void MarkDirty()
    {
        is_dirty_ = true;
    }

    [[nodiscard]] SceneNode *GetNode() const
    {
        return node_;
    }

    [[nodiscard]] const Transform &GetTransform() const
    {
        return node_->GetTransform();
    }

    [[nodiscard]] const Transform &GetLocalTransform() const
    {
        return node_->GetLocalTransform();
    }

    [[nodiscard]] bool ShouldTick() const
    {
        return always_tick_ || is_dirty_;
    }

    [[nodiscard]] bool IsRenderable() const
    {
        return is_renderable_;
    }

private:
    void SetNode(SceneNode *node)
    {
        if (node_ == node)
        {
            return;
        }

        node_ = node;
        is_dirty_ = true;
    }

    friend SceneNode;

protected:
    SceneNode *node_ = nullptr;
    uint32_t is_dirty_ : 1 = 1;
    uint32_t always_tick_ : 1 = 0;
    uint32_t is_renderable_ : 1 = 0;
    uint32_t is_attached_ : 1 = 0;
};

template <class T, typename... Args>
std::tuple<std::shared_ptr<SceneNode>, std::shared_ptr<T>> MakeNodeWithComponent(Scene *scene, SceneNode *parent_node,
                                                                                 const std::string &name,
                                                                                 Args &&...args)
{
    auto primitive_component = std::make_shared<T>(std::forward<Args>(args)...);
    auto node = std::make_shared<SceneNode>(scene, name);
    node->AddComponent(primitive_component);

    parent_node->AddChild(node);

    return std::make_tuple(node, primitive_component);
}
} // namespace sparkle
