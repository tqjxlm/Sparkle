#include "core/math/Transform.h"

#include "core/Logger.h"

namespace sparkle
{
void Transform::Update(const Vector3 &translate, const Rotation &rotation, const Vector3 &scale)
{
    ASSERT(utilities::IsNormalized(rotation));
    translation_ = translate;
    rotation_ = rotation;
    scale_ = scale;
    component_dirty_ = false;
    transform_dirty_ = true;
}

void Transform::UpdateComponents() const
{
    if (!component_dirty_)
    {
        return;
    }
    ASSERT(!transform_dirty_);

    const auto &matrix = transform_.matrix();

    translation_ = matrix.col(3).head<3>();

    scale_ = {
        matrix.col(0).head<3>().norm(),
        matrix.col(1).head<3>().norm(),
        matrix.col(2).head<3>().norm(),
    };

    Rotation::Matrix3 rotation_matrix;
    rotation_matrix.col(0) = matrix.col(0).head<3>() / scale_[0];
    rotation_matrix.col(1) = matrix.col(1).head<3>() / scale_[1];
    rotation_matrix.col(2) = matrix.col(2).head<3>() / scale_[2];

    rotation_ = rotation_matrix;

    component_dirty_ = false;
}

void Transform::Print() const
{
    Log(Info, "translation {}", utilities::VectorToString(GetTranslation()));
    Log(Info, "rotation {}", utilities::RotationToString(GetRotation()));
    Log(Info, "scale {}", utilities::VectorToString(GetScale()));
}

void Transform::UpdateTransformData() const
{
    if (!transform_dirty_)
    {
        return;
    }
    ASSERT(!component_dirty_);

    transform_ = Eigen::Translation<Scalar, 3>(translation_) * rotation_ * Eigen::Scaling(scale_);
    inv_transform_ = transform_.inverse(Eigen::TransformTraits::Affine);

    transform_dirty_ = false;
}
} // namespace sparkle
