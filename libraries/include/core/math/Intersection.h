#pragma once

#include "core/math/Ray.h"

namespace sparkle
{

class PrimitiveRenderProxy;

class Intersection
{
public:
    [[nodiscard]] auto T() const
    {
        return t_;
    }

    [[nodiscard]] Vector3 GetLocation() const
    {
        return location_;
    }

    [[nodiscard]] Vector3 GetNormal() const
    {
        return normal_;
    }

    [[nodiscard]] Vector3 GetTangent() const
    {
        return tangent_;
    }

    [[nodiscard]] Vector2 GetTexCoord() const
    {
        return tex_coord_;
    }

    [[nodiscard]] const PrimitiveRenderProxy *GetPrimitive() const
    {
        return primitive_;
    }

    [[nodiscard]] bool IsHit() const
    {
        return primitive_ != nullptr;
    }

    [[nodiscard]] bool IsCloserHit(float t) const
    {
        return t > 0 && (!IsHit() || t < t_);
    }

    void Update(const Ray &ray, const PrimitiveRenderProxy *primitive)
    {
        location_ = ray.At(t_);
        primitive_ = primitive;
    }

    void Update(const Ray &ray, const PrimitiveRenderProxy *primitive, float t, const Vector3 &normal,
                const Vector3 &tangent, const Vector2 &tex_coord = Vector2::Zero())
    {
        auto is_valid_setup = primitive_ == nullptr || t < t_;
        ASSERT(is_valid_setup);
        ASSERT(utilities::IsNormalized(normal));

        t_ = t;
        normal_ = normal;
        tangent_ = tangent;
        tex_coord_ = tex_coord;

        Update(ray, primitive);
    }

    void Invalidate()
    {
        primitive_ = nullptr;
        t_ = 0.f;
    }

    void Print() const;

private:
    const PrimitiveRenderProxy *primitive_ = nullptr;
    Vector3 location_;
    Vector3 normal_;
    Vector3 tangent_;
    Vector2 tex_coord_;
    float t_ = 0.f;
};

struct IntersectionCandidate
{
    float t = std::numeric_limits<float>::max();
    float u;
    float v;
    uint32_t face_idx;
    PrimitiveRenderProxy *primitive = nullptr;
    Vector3 geometry_normal;

    [[nodiscard]] bool IsCloserHit(float new_t) const
    {
        return new_t < t;
    }
};
} // namespace sparkle
