#pragma once

#include "core/math/Transform.h"

namespace sparkle
{

class Ray
{
public:
    explicit Ray(bool is_debug = false) : debug_(is_debug)
    {
    }

    void Reset(const Vector3 &origin, const Vector3 &direction)
    {
        ASSERT(utilities::IsNormalized(direction));

        origin_ = origin;
        direction_ = direction;
    }

    [[nodiscard]] Ray TransformedBy(const Transform &transform) const
    {
        return Ray(transform.TransformPoint(origin_), transform.TransformDirection(direction_));
    }

    [[nodiscard]] Vector3 Origin() const
    {
        return origin_;
    }

    [[nodiscard]] Vector3 Direction() const
    {
        return direction_;
    }

    [[nodiscard]] Vector3 At(float t) const
    {
        return origin_ + direction_ * t;
    }

    [[nodiscard]] float InverseAt(const Vector3 &p) const
    {
        ASSERT(utilities::IsNearlyZero((p - origin_).cross(direction_)));
        return (p - origin_).x() / direction_.x();
    }

    [[nodiscard]] bool IsDebug() const
    {
        return debug_;
    }

    void Print() const;

protected:
    Ray(Vector3 origin, Vector3 direction) : origin_(std::move(origin)), direction_(std::move(direction))
    {
    }

private:
    Vector3 origin_;
    Vector3 direction_;
    bool debug_;
};
} // namespace sparkle
