#include "renderer/proxy/CameraRenderProxy.h"

#include "core/RenderProxy.h"
#include "core/math/Utilities.h"
#include "renderer/resource/View.h"
#include "rhi/RHI.h"

namespace sparkle
{
void CameraRenderProxy::Update(RHIContext *rhi, const CameraRenderProxy &camera, const RenderConfig &config)
{
    RenderProxy::Update(rhi, camera, config);

    if (attribute_dirty_)
    {
        aspect_ratio_ = static_cast<float>(config.image_width) / static_cast<float>(config.image_height);

        auto h = std::tan(state_.vertical_fov / 2.f);
        focus_plane_.height = 2.f * h * state_.focus_distance;
        focus_plane_.width = aspect_ratio_ * focus_plane_.height;

        SetupProjectionMatrix();

        transform_dirty_ = true;
        pixels_dirty_ = true;
        attribute_dirty_ = false;
    }

    if (transform_dirty_)
    {
        OnTransformDirty(rhi);
    }

    static RenderConfig::DebugMode last_debug_rendering_mode = RenderConfig::DebugMode::Color;
    if (config.debug_mode != last_debug_rendering_mode)
    {
        pixels_dirty_ = true;
        last_debug_rendering_mode = config.debug_mode;
    }

    // view matrix is usually dirty
    ViewUBO view_ubo;
    view_ubo.view_projection_matrix = view_projection_matrix_;
    view_ubo.view_matrix = view_matrix_;
    view_ubo.projection_matrix = projection_matrix_;
    view_ubo.inv_view_matrix = view_matrix_.inverse();
    view_ubo.inv_projection_matrix = projection_matrix_.inverse();
    view_ubo.near = near_;
    view_ubo.far = far_;
    view_buffer_->Upload(rhi, &view_ubo);

    if (pixels_dirty_)
    {
        cumulated_sample_count_ = 0;
    }

    cumulated_sample_count_ =
        std::min(static_cast<unsigned>(config.max_sample_per_pixel), cumulated_sample_count_ + pending_sample_count_);

    pending_sample_count_ = 0;
}

void CameraRenderProxy::ClearPixels()
{
    ASSERT(pixels_dirty_);

    pixels_dirty_ = false;
}

void CameraRenderProxy::SetupProjectionMatrix()
{
    const float theta = state_.vertical_fov * 0.5f;
    const float inv_range = 1.f / (far_ - near_);
    const float inv_tan = 1.f / std::tan(theta);

    projection_matrix_.setZero();
    projection_matrix_(0, 0) = inv_tan / aspect_ratio_;
    projection_matrix_(1, 1) = -inv_tan;
    projection_matrix_(2, 2) = -far_ * inv_range;
    projection_matrix_(2, 3) = -near_ * far_ * inv_range;
    projection_matrix_(3, 2) = -1.f;
    projection_matrix_(3, 3) = 0;
}

void CameraRenderProxy::InitRenderResources(RHIContext *rhi, const RenderConfig &config)
{
    RenderProxy::InitRenderResources(rhi, config);

    need_cpu_frame_buffer_ = config.IsCPURenderMode();

    view_buffer_ = rhi->CreateBuffer({.size = sizeof(ViewUBO),
                                      .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                      .mem_properties = RHIMemoryProperty::None,
                                      .is_dynamic = true},
                                     "CameraViewBuffer");
}

void CameraRenderProxy::OnTransformDirty(RHIContext *rhi)
{
    RenderProxy::OnTransformDirty(rhi);

    const auto &transform = GetTransform();
    posture_.position = transform.GetTranslation();

    transform.ExtractLocalBasis(posture_.right, posture_.front, posture_.up);

    ASSERT(state_.focus_distance > 0);

    focus_plane_.max_u = posture_.right * focus_plane_.width;
    focus_plane_.max_v = posture_.up * focus_plane_.height;

    focus_plane_.lower_left = posture_.position + posture_.front * state_.focus_distance - focus_plane_.max_u * .5f -
                              focus_plane_.max_v * .5f;

    view_matrix_ = utilities::ZUpToYUpMatrix() *
                   (transform.GetRotation() * Eigen::Translation<Scalar, 3>(-posture_.position)).matrix();

    view_projection_matrix_ = projection_matrix_ * view_matrix_;

    pixels_dirty_ = true;
}

void CameraRenderProxy::Attribute::Print() const
{
    Log(Info, "camera state: vertical_fov {}, focus_distance {}, aperture_radius {}, exposure {}", vertical_fov,
        focus_distance, aperture_radius, exposure);
}
} // namespace sparkle
