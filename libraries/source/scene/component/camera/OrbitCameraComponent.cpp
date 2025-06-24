#include "scene/component/camera/OrbitCameraComponent.h"

#include "core/Logger.h"
#include "core/math/Utilities.h"

namespace Eigen
{
// NOLINTBEGIN(misc-use-internal-linkage)
// to suppress -Wctad-maybe-unsupported
template <typename T> AngleAxis(T, T) -> AngleAxis<T>;
// NOLINTEND(misc-use-internal-linkage)
} // namespace Eigen

namespace sparkle
{
void OrbitCameraComponent::UpdateTransform()
{
    const Vector3 relative_rotation = utilities::ToRadian(Vector3(-pitch_, 0.f, -yaw_));

    auto rotation_transform =
        Eigen::AngleAxis(relative_rotation.z(), Up) * Eigen::AngleAxis(relative_rotation.x(), Right);

    const Vector3 relative_direction = rotation_transform * Front;

    SetFocusDistance(radius_);

    const Vector3 position = relative_direction * -radius_ + center_;

    GetNode()->SetTransform(position, -relative_rotation);
}

void OrbitCameraComponent::SetupFromTransform()
{
    const auto &transform = GetTransform();

    const Vector3 position = transform.GetTranslation();
    const Vector3 forward = transform.TransformDirection(Front);

    // yaw: project forward onto XY plane and get angle with Y axis
    yaw_ = utilities::ToDegree(-std::atan2(forward.x(), forward.y()));

    // pitch: angle between forward and XY plane
    pitch_ = utilities::ToDegree(-std::asin(-forward.z()));

    radius_ = (center_ - position).norm();

    ASSERT(radius_ > 0.f);

    SetFocusDistance(radius_);
}

void OrbitCameraComponent::PrintPosture()
{
    Log(Info, "radius {}. yaw {}. pitch {}.", radius_, yaw_, pitch_);
}
} // namespace sparkle
