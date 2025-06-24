#pragma once

#include "scene/component/camera/CameraComponent.h"

namespace sparkle
{
// orbit camera posture is defined by center, radius, pitch, yaw.
// it always looks at center.
// pitch defines the angle between camera and xy plane.
// yaw defines the angle between camera and y axis.
// radius defines the distance between camera and center.
// it always focus on the center point, so the focus distance is always equal to radius.
class OrbitCameraComponent : public CameraComponent
{
    using CameraComponent::CameraComponent;

public:
    void Setup(const Vector3 &center, float radius, float pitch, float yaw)
    {
        radius_ = radius;
        pitch_ = pitch;
        yaw_ = yaw;
        center_ = center;

        UpdateTransform();
    }

    void SetCenter(const Vector3 &center)
    {
        center_ = center;
    }

    void SetupFromTransform();

    void OnAttach() override
    {
        CameraComponent::OnAttach();

        SetupFromTransform();
    }

    void OnPointerDown() override
    {
        is_dragging_ = true;
    }

    void OnPointerUp() override
    {
        is_dragging_ = false;
    }

    void OnPointerMove(float dx, float dy) override
    {
        if (is_dragging_)
        {
            pitch_ = std::clamp(pitch_ + dx * sensitivity_, -90.f + Tolerance, 90.f - Tolerance);
            yaw_ -= dy * sensitivity_;

            UpdateTransform();
        }
    }

    void OnScroll(float dx) override
    {
        auto new_radius = std::clamp((1.f + dx * sensitivity_) * radius_, .001f, 100.f);
        if (std::abs(radius_ - new_radius) < Eps)
        {
            return;
        }

        radius_ = new_radius;

        UpdateTransform();
    }

    void PrintPosture() override;

private:
    void UpdateTransform();

    float yaw_;
    float pitch_;
    Vector3 center_;
    float radius_;

    bool is_dragging_ = false;

    const float sensitivity_ = 0.1f;
};
} // namespace sparkle
