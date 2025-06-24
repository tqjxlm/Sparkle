#include "renderer/proxy/SphereRenderProxy.h"

namespace sparkle
{
template <bool AnyHit> bool SphereRenderProxy::IntersectInternal(const Ray &ray, IntersectionCandidate &canidate) const
{
    const Vector3 center = GetTransform().GetTranslation();

    // solve a quadratic equation
    const Vector3 center_to_origin = ray.Origin() - center;

    const float a = 1.f; // dot(dir, dir) == magnitude(dir) == 1.0f
    const float half_b = ray.Direction().dot(center_to_origin);
    const float c = center_to_origin.squaredNorm() - scaled_radius_2_;

    const float discriminant = half_b * half_b - a * c;
    if (discriminant < 0)
    {
        return false;
    }

    if constexpr (AnyHit)
    {
        // any way it must be a hit
        return true;
    }

    float sqrt_discriminant = std::sqrt(discriminant);
    float t = (-half_b + (c >= 0 ? -sqrt_discriminant : sqrt_discriminant)) / a;

    if (t > 0 && canidate.IsCloserHit(t))
    {
        canidate.t = t;
        return true;
    }

    return false;
}

bool SphereRenderProxy::IntersectAnyHit(const Ray &ray, IntersectionCandidate &canidate) const
{
    return IntersectInternal<true>(ray, canidate);
}

bool SphereRenderProxy::Intersect(const Ray &ray, IntersectionCandidate &canidate) const
{
    return IntersectInternal<false>(ray, canidate);
}
} // namespace sparkle
