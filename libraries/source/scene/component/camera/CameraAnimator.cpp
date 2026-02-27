#include "scene/component/camera/CameraAnimator.h"

#include "core/math/Utilities.h"

#include <algorithm>
#include <cmath>

namespace sparkle
{
CameraAnimator::PathType CameraAnimator::FromString(const std::string &name)
{
    if (name == "orbit_sweep")
    {
        return PathType::kOrbitSweep;
    }
    if (name == "dolly")
    {
        return PathType::kDolly;
    }
    return PathType::kNone;
}

void CameraAnimator::Setup(PathType type, uint32_t total_frames, const OrbitPose &initial_pose)
{
    path_type_ = type;
    total_frames_ = std::max(total_frames, 1u);
    initial_pose_ = initial_pose;
}

CameraAnimator::OrbitPose CameraAnimator::GetPose(uint32_t frame_index) const
{
    OrbitPose pose = initial_pose_;
    float t = static_cast<float>(frame_index) / static_cast<float>(total_frames_);

    switch (path_type_)
    {
    case PathType::kOrbitSweep: {
        // Full 360° yaw sweep over total_frames
        pose.yaw = initial_pose_.yaw + t * 360.0f;
        break;
    }
    case PathType::kDolly: {
        // Sinusoidal forward/back along radius (±30% of initial radius)
        float offset = std::sin(t * 2.0f * Pi) * initial_pose_.radius * 0.3f;
        pose.radius = std::max(initial_pose_.radius - offset, 0.1f);
        break;
    }
    default:
        break;
    }

    return pose;
}
} // namespace sparkle
