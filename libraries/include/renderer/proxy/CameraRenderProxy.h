#pragma once

#include "core/RenderProxy.h"

#include "rhi/RHIBuffer.h"
#include "scene/component/camera/CameraComponent.h"

namespace sparkle
{
class Ray;
class SceneRenderProxy;
class Image2D;

// the camera render proxy is used for:
// 1. translate camera parameters to view info.
// 2. view info management.
// 3. main entrance for CPURenderer pipeline core algorithm..
// TODO(tqjxlm) separate view from camera
class CameraRenderProxy : public RenderProxy
{
public:
    using ColorBuffer = std::vector<std::vector<Vector4>>;

    static constexpr Scalar OutputLimit = 6.f;

    struct UniformBufferData
    {
        alignas(16) Vector3 position;
        uint32_t mode;
        alignas(16) Vector3 lowerLeft;
        uint32_t max_bounce;
        alignas(16) Vector3 max_u;
        alignas(16) Vector3 max_v;
        float lensRadius;
        alignas(16) Vector2Int resolution;
    };

    struct GBuffer
    {
        // holds one frame's color output. alpha channel: whether this pixel is valid
        ColorBuffer color;

        // holds one frame's normal output
        std::vector<std::vector<Vector3>> world_normal;

        [[nodiscard]] bool IsValid(unsigned i, unsigned j) const
        {
            return color[j][i].w() > 0;
        }

        [[nodiscard]] bool IsSky(unsigned i, unsigned j) const
        {
            return IsValid(i, j) && world_normal[j][i].isZero();
        }

        void Resize(unsigned width, unsigned height)
        {
            color.resize(height, std::vector<Vector4>(width));
            world_normal.resize(height, std::vector<Vector3>(width));
        }

        void Clear()
        {
            color.clear();
            world_normal.clear();
        }
    };

    void Update(RHIContext *rhi, const CameraRenderProxy &camera, const RenderConfig &config) override;

    void InitRenderResources(RHIContext *rhi, const RenderConfig &config) override;

    void OnTransformDirty(RHIContext *rhi) override;

    void RenderCPU(const SceneRenderProxy &scene, const RenderConfig &config, const Vector2UInt &debug_point);

    void Print(Image2D &image);

    void ClearPixels();

    void SetData(const CameraComponent::CameraState &state)
    {
        state_ = state;
        data_dirty_ = true;
    }

    void MarkPixelDirty()
    {
        pixels_dirty_ = true;
    }

    void AccumulateSample(uint32_t sample_count)
    {
        pending_sample_count_ += sample_count;
    }

    [[nodiscard]] RHIResourceRef<RHIBuffer> GetViewBuffer() const
    {
        return view_buffer_;
    }

    [[nodiscard]] bool NeedClear() const
    {
        return pixels_dirty_;
    }

    [[nodiscard]] auto GetAttribute() const
    {
        return state_;
    }

    [[nodiscard]] uint32_t GetCumulatedSampleCount() const
    {
        return cumulated_sample_count_;
    }

    [[nodiscard]] Vector3 GetTranslation() const
    {
        return position_;
    }

    [[nodiscard]] TransformMatrix GetViewMatrix() const
    {
        return view_matrix_;
    }

    [[nodiscard]] Mat4 GetProjectionMatrix() const
    {
        return projection_matrix_;
    }

    [[nodiscard]] Mat4 GetViewProjectionMatrix() const
    {
        return view_projection_matrix_;
    }

    [[nodiscard]] float GetNear() const
    {
        return near_;
    }

    [[nodiscard]] float GetFar() const
    {
        return far_;
    }

    [[nodiscard]] float GetAspectRatio() const
    {
        return static_cast<float>(image_size_.x()) / static_cast<float>(image_size_.y());
    }

    [[nodiscard]] UniformBufferData GetUniformBufferData(const RenderConfig &config) const
    {
        return {.position = position_,
                .mode = static_cast<uint32_t>(config.debug_mode),
                .lowerLeft = focus_plane_.lower_left,
                .max_bounce = static_cast<uint32_t>(config.max_bounce),
                .max_u = focus_plane_.max_u,
                .max_v = focus_plane_.max_v,
                .lensRadius = state_.aperture_radius,
                .resolution = {config.image_width, config.image_height}};
    }

private:
    struct SampleResult
    {
        Vector3 color = Zeros;
        Vector3 world_normal = Zeros;
        float valid_flag = 1.f;
    };

    void RenderPixel(unsigned i, unsigned j, Scalar pixel_width, Scalar pixel_height, const SceneRenderProxy &scene,
                     const RenderConfig &config, const Vector2UInt &debug_point);

    void BasePass(const SceneRenderProxy &scene, const RenderConfig &config, const Vector2UInt &debug_point);

    void DenoisePass(const RenderConfig &config, const Vector2UInt &debug_point);

    [[nodiscard]] SampleResult SamplePixel(const SceneRenderProxy &scene, const RenderConfig &config, float u, float v,
                                           bool debug) const;

    void SetupViewRay(Ray &ray, float u, float v) const;

    [[nodiscard]] Vector3 ToneMapping(const Vector3 &color) const;

    void SetupProjectionMatrix();

    // output of rendering passes. cleared every frame.
    GBuffer gbuffer_;

    // a temporary color buffer to store color values. cleared every frame
    ColorBuffer ping_pong_buffer_;

    // accumulates all frame's results after temporal denoising. cleared on dirty
    ColorBuffer frame_buffer_;

    // cached from camera component
    CameraComponent::CameraState state_;

    TransformMatrix view_matrix_;
    Mat4 projection_matrix_;
    Mat4 view_projection_matrix_;

    // camera's lens center position in world space
    Vector3 position_;

    Vector3 up_;
    Vector3 front_;
    Vector3 right_;

    // focus plane in world space
    struct
    {
        float height;
        float width;
        Vector3 max_u;
        Vector3 max_v;
        Vector3 lower_left;
    } focus_plane_;

    Vector2UInt image_size_;

    float near_ = 0.1f;
    float far_ = 1000.f;

    unsigned pending_sample_count_ = 0;
    unsigned cumulated_sample_count_ = 0;
    unsigned next_cumulative_sample_ = 0;
    unsigned sub_pixel_count_;
    unsigned actual_sample_per_pixel_;

    uint64_t total_frame_ = 0;

    RHIResourceRef<RHIBuffer> view_buffer_;

    uint32_t pixels_dirty_ : 1 = 1;
    uint32_t data_dirty_ : 1 = 1;
    uint32_t need_cpu_frame_buffer_ : 1 = 0;
};
} // namespace sparkle
