#include "renderer/proxy/DirectionalLightRenderProxy.h"

namespace sparkle
{
DirectionalLightRenderProxy::DirectionalLightRenderProxy()
{
    if (cast_shadow_)
    {
        const float inv_view_z_range = 1.f / (shadow_far_ - shadow_near_);
        const float inv_view_x_range = 1.f / shadow_view_width_;
        const float inv_view_y_range = 1.f / shadow_view_height_;

        shadow_projection_matrix_.setZero();
        shadow_projection_matrix_(0, 0) = inv_view_x_range;
        // vulkan invert Y
        shadow_projection_matrix_(1, 1) = -inv_view_y_range;
        // gl maps depth to (-1, 1), vulkan maps depth to (0, 1)
        shadow_projection_matrix_(2, 2) = -inv_view_z_range;
        shadow_projection_matrix_(2, 3) = -shadow_near_ * inv_view_z_range;
        shadow_projection_matrix_(3, 3) = 1.f;
    }
}

void DirectionalLightRenderProxy::UpdateMatrices(const Vector3 &direciton)
{
    if (!cast_shadow_)
    {
        return;
    }

    ubo_.direction = direciton;

    const Vector3 light_front = -ubo_.direction;
    const Vector3 light_right = light_front.cross(Up).normalized();
    const Vector3 light_up = light_right.cross(light_front).normalized();
    const Vector3 eye = ubo_.direction * 30.f;

    shadow_view_matrix_.row(0) = utilities::ConcatVector(light_right, light_right.dot(-eye));
    shadow_view_matrix_.row(1) = utilities::ConcatVector(light_front, light_front.dot(-eye));
    shadow_view_matrix_.row(2) = utilities::ConcatVector(light_up, light_up.dot(-eye));
    shadow_view_matrix_.row(3) = Vector4(0, 0, 0, 1.f);

    ubo_.shadow_matrix = shadow_projection_matrix_ * utilities::ZUpToYUpMatrix() * shadow_view_matrix_;
}
} // namespace sparkle
