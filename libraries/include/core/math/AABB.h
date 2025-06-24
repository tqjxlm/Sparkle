#pragma once

#include "core/math/Transform.h"

namespace sparkle
{
class AABB
{
    Vector3 center_;
    Vector3 half_size_ = {-1.f, -1.f, -1.f};

public:
    AABB() = default;

    AABB(Vector3 center, Vector3 half_size) : center_(std::move(center)), half_size_(std::move(half_size))
    {
        ASSERT(IsValid());
    }

    [[nodiscard]] bool IsValid() const
    {
        return (half_size_.array() >= Zeros.array()).all();
    }

    [[nodiscard]] Vector3 Center() const
    {
        return center_;
    }

    [[nodiscard]] Vector3 HalfSize() const
    {
        return half_size_;
    }

    [[nodiscard]] Vector3 Size() const
    {
        return half_size_ * 2.f;
    }

    [[nodiscard]] Vector3 Min() const
    {
        return center_ - half_size_;
    }

    [[nodiscard]] Vector3 Max() const
    {
        return center_ + half_size_;
    }

    [[nodiscard]] bool Intersect(const AABB &other) const
    {
        auto distance = (center_ - other.center_).cwiseAbs();
        auto min_distance = half_size_ + other.half_size_;
        return (distance.array() < min_distance.array()).all();
    }

    [[nodiscard]] AABB TransformTo(const Transform &transform) const
    {
        Vector3 min_corner = Ones * std::numeric_limits<Scalar>::max();
        Vector3 max_corner = Ones * -std::numeric_limits<Scalar>::max();

        static const Vector3 Directions[8] = {Vector3(1, 1, 1),   Vector3(-1, 1, 1),  Vector3(1, -1, 1),
                                              Vector3(1, 1, -1),  Vector3(-1, -1, 1), Vector3(1, -1, -1),
                                              Vector3(-1, 1, -1), Vector3(-1, -1, -1)};

        for (const auto &direction : Directions)
        {
            auto local_corner = center_ + half_size_.cwiseProduct(direction);
            const Vector3 world_corner = transform.TransformPoint(local_corner);
            min_corner = min_corner.cwiseMin(world_corner);
            max_corner = max_corner.cwiseMax(world_corner);
        }

        auto center = (min_corner + max_corner) / 2.f;
        auto size = (max_corner - center).cwiseAbs();

        return {center, size};
    }

    AABB operator+(const AABB &other)
    {
        if (!other.IsValid())
        {
            return *this;
        }

        Vector3 min = (center_ - half_size_).cwiseMin(other.center_ - other.half_size_);
        Vector3 max = (center_ + half_size_).cwiseMax(other.center_ + other.half_size_);
        Vector3 center = (min + max) * 0.5f;
        Vector3 half_size = (max - min) * 0.5f;

        return {center, half_size};
    }

    AABB &operator+=(const AABB &other)
    {
        if (!other.IsValid())
        {
            return *this;
        }

        *this = *this + other;
        return *this;
    }

    [[nodiscard]] std::string ToString() const;
};
} // namespace sparkle
