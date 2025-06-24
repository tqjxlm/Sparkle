#pragma once

#include "core/math/Utilities.h"

namespace sparkle
{
class Transform
{
public:
    Transform(Vector3 translate, Rotation rotation, Vector3 scale)
        : translation_(std::move(translate)), rotation_(std::move(rotation)), scale_(std::move(scale)),
          component_dirty_(false)
    {
    }

    Transform() : Transform(Zeros, Zeros, Ones)
    {
    }

    Transform(const Vector3 &translate, const Vector3 &rotation, const Vector3 &scale)
        : Transform(translate, utilities::EulerRotationToRotationAxis(rotation), scale)
    {
    }

    explicit Transform(TransformData transform_data, TransformData inv_transform_data)
        : transform_(std::move(transform_data)), inv_transform_(std::move(inv_transform_data)), transform_dirty_(false)
    {
    }

    void Update(const Mat4x4 &matrix)
    {
        transform_.matrix() = matrix;
        inv_transform_ = transform_.inverse();
        component_dirty_ = true;
        transform_dirty_ = false;
    }

    void Update(const Vector3 &translate, const Vector3 &rotation, const Vector3 &scale)
    {
        Update(translate, utilities::EulerRotationToRotationAxis(rotation), scale);
    }

    void Update(const Vector3 &translate, const Rotation &rotation, const Vector3 &scale);

    [[nodiscard]] const Vector3 &GetTranslation() const
    {
        [[unlikely]] if (component_dirty_)
        {
            UpdateComponents();
        }
        return translation_;
    }

    [[nodiscard]] const Rotation &GetRotation() const
    {
        [[unlikely]] if (component_dirty_)
        {
            UpdateComponents();
        }
        return rotation_;
    }

    [[nodiscard]] const Vector3 &GetScale() const
    {
        [[unlikely]] if (component_dirty_)
        {
            UpdateComponents();
        }
        return scale_;
    }

    [[nodiscard]] const TransformData &GetTransformData() const
    {
        [[unlikely]] if (transform_dirty_)
        {
            UpdateTransformData();
        }
        return transform_;
    }

    [[nodiscard]] const TransformData &GetInvTransformData() const
    {
        [[unlikely]] if (transform_dirty_)
        {
            UpdateTransformData();
        }
        return inv_transform_;
    }

    [[nodiscard]] const auto &GetMatrix() const
    {
        return GetTransformData().matrix();
    }

    [[nodiscard]] Vector3 TransformPoint(const Vector3 &point) const
    {
        return GetTransformData() * point;
    }

    [[nodiscard]] Vector3 TransformDirection(const Vector3 &direction) const
    {
        return GetMatrix().topLeftCorner<3, 3>() * direction;
    }

    [[nodiscard]] Vector3 TransformDirectionTangentSpace(const Vector3 &direction) const
    {
        return GetInverse().GetMatrix().topLeftCorner<3, 3>() * direction;
    }

    [[nodiscard]] Transform GetInverse() const
    {
        [[likely]] if (!transform_dirty_)
        {
            return Transform(inv_transform_, transform_);
        }

        ASSERT(!component_dirty_);
        return Transform(-translation_, rotation_.conjugate(), scale_.cwiseInverse());
    }

    void ExtractLocalBasis(Vector3 &x, Vector3 &y, Vector3 &z) const
    {
        x = GetMatrix().row(0).head<3>();
        y = GetMatrix().row(1).head<3>();
        z = GetMatrix().row(2).head<3>();
    }

    void Print() const;

private:
    void UpdateTransformData() const;

    void UpdateComponents() const;

    // affine components that define this transform
    mutable Vector3 translation_;
    mutable Rotation rotation_;
    mutable Vector3 scale_;

    // transforms used in matrix calculation
    mutable TransformData transform_;
    mutable TransformData inv_transform_;

    // if component dirty, we extract them from transform in UpdateComponents
    mutable bool component_dirty_ = true;
    // if transform dirty, we construct them from components in UpdateTransformData
    mutable bool transform_dirty_ = true;
};
} // namespace sparkle
