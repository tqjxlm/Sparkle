#pragma once

#include "core/RenderProxy.h"

#include "core/math/AABB.h"

namespace sparkle
{
class Ray;
class Intersection;
struct IntersectionCandidate;
class MaterialRenderProxy;

class PrimitiveRenderProxy : public RenderProxy
{
public:
    explicit PrimitiveRenderProxy(std::string_view name, AABB local_bound);

    ~PrimitiveRenderProxy() override;

    void Update(RHIContext *rhi, const CameraRenderProxy &camera, const RenderConfig &config) override;

    void OnTransformDirty(RHIContext *rhi) override;

    [[nodiscard]] AABB GetWorldBoundingBox() const
    {
        return world_bound_;
    }

    void SetMaterialRenderProxy(MaterialRenderProxy *material)
    {
        material_proxy_ = material;
    }

    [[nodiscard]] MaterialRenderProxy *GetMaterialRenderProxy() const
    {
        return material_proxy_;
    }

    [[nodiscard]] uint32_t GetPrimitiveIndex() const
    {
        return primitive_index_;
    }

    void SetPrimitiveIndex(uint32_t primitive_index)
    {
        primitive_index_ = primitive_index;
    }

    virtual bool Intersect(const Ray &ray, IntersectionCandidate &canidate) const = 0;

    virtual bool IntersectAnyHit(const Ray &ray, IntersectionCandidate &candidate) const = 0;

    virtual void BuildBVH()
    {
    }

    virtual void GetIntersection([[maybe_unused]] const Ray &ray,
                                 [[maybe_unused]] const IntersectionCandidate &candidate,
                                 [[maybe_unused]] Intersection &intersection)
    {
    }

    [[nodiscard]] const auto &GetName() const
    {
        return name_;
    }

protected:
    MaterialRenderProxy *material_proxy_ = nullptr;

private:
    uint32_t primitive_index_ = UINT_MAX;

    AABB local_bound_;
    AABB world_bound_;

    std::string_view name_;
};
} // namespace sparkle
