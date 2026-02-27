#pragma once

#include "core/math/Types.h"

#include <string>

namespace sparkle
{
class CameraAnimator
{
public:
    enum class PathType : uint8_t
    {
        kNone,
        kOrbitSweep,
        kDolly
    };

    static PathType FromString(const std::string &name);

    struct OrbitPose
    {
        Vector3 center;
        float radius;
        float pitch; // degrees
        float yaw;   // degrees
    };

    void Setup(PathType type, uint32_t total_frames, const OrbitPose &initial_pose);

    [[nodiscard]] OrbitPose GetPose(uint32_t frame_index) const;

    [[nodiscard]] bool IsActive() const
    {
        return path_type_ != PathType::kNone;
    }

private:
    PathType path_type_ = PathType::kNone;
    uint32_t total_frames_ = 1;
    OrbitPose initial_pose_{};
};
} // namespace sparkle
