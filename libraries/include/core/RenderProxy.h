#pragma once

#include "core/math/Transform.h"

#include "renderer/RenderConfig.h"

namespace sparkle
{
class CameraRenderProxy;
class RHIContext;

class RenderProxy
{
public:
    virtual ~RenderProxy() = default;

    virtual void Update(RHIContext *rhi, [[maybe_unused]] const CameraRenderProxy &camera, const RenderConfig &config)
    {
        if (rhi_dirty_)
        {
            InitRenderResources(rhi, config);
        }
    }

    virtual void InitRenderResources([[maybe_unused]] RHIContext *rhi, [[maybe_unused]] const RenderConfig &config)
    {
        rhi_dirty_ = false;
    }

    virtual void OnTransformDirty(RHIContext *)
    {
        transform_dirty_ = false;
    }

    void UpdateTransform(const Transform &transform)
    {
        transform_ = transform;
        transform_dirty_ = true;
    }

    [[nodiscard]] Transform GetTransform() const
    {
        return transform_;
    }

    [[nodiscard]] bool IsPrimitive() const
    {
        return is_primitive_;
    }

    [[nodiscard]] bool IsMesh() const
    {
        return is_mesh_;
    }

    [[nodiscard]] bool IsLight() const
    {
        return is_light_;
    }

    [[nodiscard]] bool IsRHIDirty() const
    {
        return rhi_dirty_;
    }

    // CAUTION: we do not validate this cast! make sure you know what you are doing.
    template <class T> T *As()
    {
        return static_cast<T *>(this);
    }

    [[nodiscard]] auto GetIndex() const
    {
        return index_;
    }

    void SetIndex(uint32_t index)
    {
        index_ = index;
    }

protected:
    Transform transform_;

    // index in the scene render proxy
    uint32_t index_ = UINT_MAX;

    uint32_t is_mesh_ : 1 = 0;
    uint32_t is_primitive_ : 1 = 0;
    uint32_t is_light_ : 1 = 0;
    uint32_t transform_dirty_ : 1 = 1;
    uint32_t rhi_dirty_ : 1 = 1;
};
} // namespace sparkle
