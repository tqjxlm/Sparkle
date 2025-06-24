#pragma once

#include "renderer/proxy/MeshRenderProxy.h"

#include "core/math/Intersection.h"

namespace sparkle
{
class SphereRenderProxy : public MeshRenderProxy
{
public:
    explicit SphereRenderProxy(const std::shared_ptr<const Mesh> &raw_mesh, std::string_view name,
                               const AABB &local_bound)
        : MeshRenderProxy(raw_mesh, name, local_bound)
    {
    }

    bool Intersect(const Ray &ray, IntersectionCandidate &canidate) const override;

    bool IntersectAnyHit(const Ray &ray, IntersectionCandidate &canidate) const override;

    void GetIntersection(const Ray &ray, const IntersectionCandidate &candidate, Intersection &intersection) override
    {
        Vector3 center = GetTransform().GetTranslation();

        Vector3 p = ray.At(candidate.t);
        Vector3 normal = (p - center).normalized();
        Vector3 tangent = utilities::GetPossibleMajorAxis(normal);

        intersection.Update(ray, this, candidate.t, normal, tangent);
    }

    void SetRadius(float radius)
    {
        scaled_radius_2_ = radius * radius;
    }

private:
    template <bool AnyHit> bool IntersectInternal(const Ray &ray, IntersectionCandidate &canidate) const;

    float scaled_radius_2_;
};
} // namespace sparkle
