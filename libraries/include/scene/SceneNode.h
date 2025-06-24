#pragma once

#include "core/math/Transform.h"

#include <string_view>
#include <vector>

namespace sparkle
{
class Component;
class Scene;

class SceneNode
{
public:
    explicit SceneNode(Scene *scene, std::string name) : scene_(scene), name_(std::move(name))
    {
        ASSERT(!name_.empty());
    }

    void Tick();

    void SetName(std::string name)
    {
        name_ = std::move(name);
    }

    [[nodiscard]] std::string_view GetName() const
    {
        return name_;
    }

    Scene *GetScene()
    {
        return scene_;
    }

#pragma region SceneGraph

    void AddChild(const std::shared_ptr<SceneNode> &child);

    void RemoveChild(const std::shared_ptr<SceneNode> &child);

    [[nodiscard]] const std::vector<std::shared_ptr<SceneNode>> &GetChildren() const
    {
        return children_;
    }

#pragma endregion

#pragma region Component

    [[nodiscard]] const std::vector<std::shared_ptr<Component>> &GetComponents() const
    {
        return components_;
    }

    void AddComponent(const std::shared_ptr<Component> &component);

#pragma endregion

#pragma region Transform

    void SetTransform(const Mat4x4 &matrix)
    {
        local_transform_.Update(matrix);

        MarkTransformDirty();
    }

    void SetTransform(const Vector3 &translate, const Vector3 &rotation = Zeros, const Vector3 &scale = Ones)
    {
        local_transform_.Update(translate, rotation, scale);

        MarkTransformDirty();
    }

    void SetTransform(const Vector3 &translate, const Rotation &rotation, const Vector3 &scale = Ones)
    {
        local_transform_.Update(translate, rotation, scale);

        MarkTransformDirty();
    }

    const Transform &GetTransform()
    {
        if (transform_dirty_)
        {
            UpdateDirtyTransform();
        }
        return world_transform_;
    }

    [[nodiscard]] const Transform &GetLocalTransform() const
    {
        return local_transform_;
    }

    [[nodiscard]] bool IsTransformDirty() const
    {
        return transform_dirty_;
    }

    void UpdateDirtyTransform();

    void Traverse(const std::function<void(SceneNode *)> &func)
    {
        func(this);

        for (auto &child : children_)
        {
            child->Traverse(func);
        }
    }

    SceneNode *GetRootNode()
    {
        SceneNode *cur = this;
        while (cur->parent_)
        {
            cur = cur->parent_;
        }

        return cur;
    }

    const SceneNode *GetRootNode() const
    {
        const auto *cur = this;
        while (cur->parent_)
        {
            cur = cur->parent_;
        }

        return cur;
    }

    bool IsInScene() const;

#pragma endregion

private:
    void MarkTransformDirty();

    void OnAttachToScene();

    Transform local_transform_;

    Transform world_transform_;

    Scene *scene_ = nullptr;

    SceneNode *parent_ = nullptr;

    std::string name_;

    std::vector<std::shared_ptr<SceneNode>> children_;

    std::vector<std::shared_ptr<Component>> components_;

    // index of this node in its parent's children
    uint32_t sibling_index_ = UINT_MAX;

    uint32_t transform_dirty_ : 1 = 1u;
};
} // namespace sparkle
