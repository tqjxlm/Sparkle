#pragma once

#include "renderer/proxy/LightRenderProxy.h"

#include "core/math/Ray.h"

namespace sparkle
{
class DirectionalLightRenderProxy : public LightRenderProxy
{
public:
    DirectionalLightRenderProxy();

    struct UniformBufferData
    {
        alignas(16) Vector3 color = Ones;
        float shadow_depth_bias = 1e-6f;
        alignas(16) Vector3 direction = {0, std::cos(utilities::ToRadian(20.f)), std::sin(utilities::ToRadian(45.f))};
        float shadow_normal_bias = 0.001f;
        Mat4 shadow_matrix;
    };

    void SetColor(const Vector3 &color)
    {
        ubo_.color = color;
    }

    void UpdateMatrices(const Vector3 &direciton);

    [[nodiscard]] UniformBufferData GetRenderData() const
    {
        return ubo_;
    }

    [[nodiscard]] const Mat4 &GetShadowViewMatrix() const
    {
        return shadow_view_matrix_;
    }

    [[nodiscard]] const Mat4 &GetShadowProjectionMatrix() const
    {
        return shadow_projection_matrix_;
    }

#pragma region CPU Render

    [[nodiscard]] Vector3 Evaluate(const Ray &ray) const override
    {
        return std::clamp(ray.Direction().dot(ubo_.direction), 0.f, 1.f) * ubo_.color;
    }

    void Sample(const Vector3 & /*origin*/, Vector3 &direction) const override
    {
        direction = ubo_.direction;
    }

#pragma endregion

private:
    Mat4 shadow_projection_matrix_;
    Mat4 shadow_view_matrix_;
    Mat4 shadow_view_projection_matrix_;

    UniformBufferData ubo_;

    // left, right, bottm, top
    Vector4 shadow_viewport_ = Vector4(-1.f, 1.f, -1.f, 1.f) * 10.f;
    Scalar shadow_near_ = 0.1f;
    Scalar shadow_far_ = 1000.f;
    Scalar shadow_view_width_ = 20.f;
    Scalar shadow_view_height_ = 20.f;

    bool cast_shadow_ = true;
};
} // namespace sparkle
