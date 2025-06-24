#include "renderer/proxy/CameraRenderProxy.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "core/RenderProxy.h"
#include "core/TaskManager.h"
#include "core/math/Intersection.h"
#include "core/math/Ray.h"
#include "core/math/Sampler.h"
#include "core/math/Utilities.h"
#include "renderer/proxy/MaterialRenderProxy.h"
#include "renderer/proxy/PrimitiveRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/View.h"
#include "rhi/RHI.h"

// RR improves performance but causes too much noise, disable for now
#define RUSSIAN_ROULETTE 0

namespace sparkle
{
CameraRenderProxy::SampleResult CameraRenderProxy::SamplePixel(const SceneRenderProxy &scene,
                                                               const RenderConfig &config, float u, float v,
                                                               bool debug) const
{
    Ray ray(debug);
    SetupViewRay(ray, u, v);

    Vector3 throughput = Ones;
    Intersection intersection;

    SampleResult result;

    const auto *sky_light = scene.GetSkyLight();

    Vector3 debug_color = Zeros;

    auto max_bounce = static_cast<unsigned>(config.max_bounce);
    unsigned bounce = 0;
    for (; bounce < max_bounce; bounce++)
    {
        scene.Intersect<false>(ray, intersection);

        // terminal condition: hit nothing
        if (!intersection.IsHit())
        {
            if (sky_light)
            {
                result.color += sky_light->Evaluate(ray).cwiseProduct(throughput);
            }

            break;
        }

        const auto *primitive = intersection.GetPrimitive();
        const auto *material = primitive->GetMaterialRenderProxy();
        Vector2 tex_coord = intersection.GetTexCoord();
        Vector3 hit_normal = intersection.GetNormal();
        Vector3 hit_tangent = intersection.GetTangent();

        if (bounce == 0)
        {
            result.world_normal = hit_normal;
        }

        // terminal condition: emissive
        Vector3 emissive_color = material->GetEmissive(tex_coord);
        [[unlikely]] if (emissive_color.norm() > Eps)
        {
            result.color += emissive_color.cwiseProduct(throughput);
            break;
        }

        // core procedure: surface sampling
        Vector3 next_direction = Zeros;
        Vector3 this_throughput = material->SampleSurface(ray, next_direction, hit_normal, hit_tangent, tex_coord);

        // terminal condition: debug output
        switch (config.debug_mode)
        {
        case RenderConfig::DebugMode::Debug:
            result.color = debug_color;
            return result;
        case RenderConfig::DebugMode::Normal:
            result.color = utilities::VisualizeVector(hit_normal);
            return result;
        case RenderConfig::DebugMode::RayDirection:
            result.color = utilities::VisualizeVector(next_direction);
            return result;
        case RenderConfig::DebugMode::Metallic:
            result.color = Ones * material->GetMetallic(tex_coord);
            return result;
        case RenderConfig::DebugMode::Roughness:
            result.color = Ones * material->GetRoughness(tex_coord);
            return result;
        case RenderConfig::DebugMode::Albedo:
            result.color = material->GetBaseColor(tex_coord);
            return result;
        case RenderConfig::DebugMode::Emissive:
            result.color = material->GetEmissive(tex_coord);
            return result;
        case RenderConfig::DebugMode::Depth:
            result.color = Ones * (intersection.GetLocation() - position_).dot(front_) / far_;
            return result;
        [[likely]] default:
            break;
        }

        // core procedure: radiance decay every bounce
        throughput = throughput.cwiseProduct(this_throughput);

#if RUSSIAN_ROULETTE
        // russian roulette
        auto discard_probabiity = throughput.norm();
        auto discard_roll = sampler::RandomUnit();
        if (discard_roll > discard_probabiity)
        {
            break;
        }

        throughput /= discard_probabiity;
#else
        // terminal condition: early out
        if (throughput.squaredNorm() < Eps)
        {
            result.valid_flag = -1.f;
            break;
        }
#endif

        [[unlikely]] if (ray.IsDebug())
        {
            Log(Warn, "Hit bounce {}. This throughput {}. Throughput {}. Next direction {}", bounce,
                utilities::VectorToString(this_throughput), utilities::VectorToString(throughput),
                utilities::VectorToString(next_direction));
            ray.Print();
            intersection.Print();
            material->PrintSample(tex_coord);
        }

        // Russian roulette
        if (bounce >= 3)
        {
            Scalar p = throughput.maxCoeff();

            // Avoid too low probability. It will introduce a small bias.
            p = std::clamp(p, 0.05f, 1.0f);

            if (sampler::RandomUnit() > p)
            {
                break;
            }

            // Compensate for survival probability
            throughput /= p;
        }

        // next ray
        ray.Reset(intersection.GetLocation() + next_direction * Tolerance, next_direction);
        intersection.Invalidate();
    }

    switch (config.debug_mode)
    {
    case RenderConfig::DebugMode::RayDepth:
        result.color = utilities::VisualizeInteger(bounce);
        break;
    case RenderConfig::DebugMode::Debug:
        result.color = debug_color;
        break;
    case RenderConfig::DebugMode::IndirectLighting:
        if (bounce <= 1)
        {
            result.color = Zeros;
        }
        break;
    case RenderConfig::DebugMode::DirectionalLighting:
        if (bounce > 1)
        {
            result.color = Zeros;
        }
        break;
    [[likely]] default:
        break;
    }

    return result;
}

void CameraRenderProxy::RenderPixel(unsigned i, unsigned j, Scalar pixel_width, Scalar pixel_height,
                                    const SceneRenderProxy &scene, const RenderConfig &config,
                                    const Vector2UInt &debug_point)
{
    const bool is_debug = i == debug_point.x() && j == debug_point.y();

    auto u = (static_cast<float>(i) + sampler::RandomUnit()) * pixel_width;
    auto v = (static_cast<float>(j) + sampler::RandomUnit()) * pixel_height;

    auto result = SamplePixel(scene, config, u, v, is_debug);

    result.color = result.color.cwiseMin(Ones * OutputLimit);

    gbuffer_.color[j][i].head<3>() = result.color;
    gbuffer_.color[j][i].w() = result.valid_flag;

    if (config.debug_mode == RenderConfig::DebugMode::Color && config.spatial_denoise)
    {
        gbuffer_.world_normal[j][i] = result.world_normal;
    }
}

void CameraRenderProxy::BasePass(const SceneRenderProxy &scene, const RenderConfig &config,
                                 const Vector2UInt &debug_point)
{
    PROFILE_SCOPE("camera base pass");

    const float pixel_width = 1.f / static_cast<float>(image_size_.x() - 1);
    const float pixel_height = 1.f / static_cast<float>(image_size_.y() - 1);

    static std::vector<std::future<void>> row_tasks;
    row_tasks.resize(image_size_.y());

    // parallel by row
    for (auto j = 0u; j < image_size_.y(); j++)
    {
        row_tasks[j] = TaskManager::RunInWorkerThread([=, this, &scene]() {
            for (auto i = 0u; i < image_size_.x(); i++)
            {
                RenderPixel(i, j, pixel_width, pixel_height, scene, config, debug_point);
            }
        });
    }

    for (const auto &row_task : row_tasks)
    {
        row_task.wait();
    }
}

static void SpatialDenoisePixel(unsigned i, unsigned j, unsigned width, unsigned height, unsigned num_samples,
                                const CameraRenderProxy::GBuffer &gbuffer,
                                std::vector<std::vector<Vector4>> &output_buffer)
{
    const static std::array<std::tuple<int, int>, 8> Directions{
        std::tuple<int, int>{1, 0},  std::tuple<int, int>{0, 1},   std::tuple<int, int>{-1, 0},
        std::tuple<int, int>{0, -1}, std::tuple<int, int>{-1, -1}, std::tuple<int, int>{1, -1},
        std::tuple<int, int>{-1, 1}, std::tuple<int, int>{1, 1}};

    const Vector3 &world_normal = gbuffer.world_normal[j][i];
    const Vector3 &color = gbuffer.color[j][i].head<3>();

    // only a valid non-sky intersection may be used as reference
    if (!gbuffer.IsValid(i, j) || gbuffer.IsSky(i, j))
    {
        return;
    }

    // fill surrounding invalid samples with current sample
    for (auto k = 0u; k < std::min(num_samples, static_cast<unsigned>(Directions.size())); k++)
    {
        auto [dx, dy] = Directions[k];
        int sample_i = static_cast<int>(i) + dx;
        int sample_j = static_cast<int>(j) + dy;
        if (sample_i < 0 || sample_i >= static_cast<int>(width) || sample_j < 0 || sample_j >= static_cast<int>(height))
        {
            continue;
        }

        auto new_i = static_cast<unsigned>(sample_i);
        auto new_j = static_cast<unsigned>(sample_j);

        const Vector3 &neighbour_normal = gbuffer.world_normal[new_j][new_i];
        float neighbour_valid_flag = gbuffer.color[new_j][new_i].w();
        if (neighbour_valid_flag > 0)
        {
            continue;
        }

        auto similarity = neighbour_normal.dot(world_normal);
        if (similarity < 0.99f)
        {
            continue;
        }

        output_buffer[new_j][new_i].head<3>() = color;

        // now assume this pixel has a valid value
        output_buffer[new_j][new_i].w() = 1.f;

        break;
    }
}

void CameraRenderProxy::DenoisePass(const RenderConfig &config, const Vector2UInt &debug_point)
{
    PROFILE_SCOPE("camera denoise pass");

    // spatial denoise
    if (config.spatial_denoise)
    {
        TaskManager::ParallelFor(0u, image_size_.y(), [this](unsigned j) {
            for (auto i = 0u; i < image_size_.x(); i++)
            {
                ping_pong_buffer_[j][i] = gbuffer_.color[j][i];
            }
        }).wait();

        TaskManager::ParallelFor(0u, image_size_.y(), [this](unsigned j) {
            for (auto i = 0u; i < image_size_.x(); i++)
            {
                SpatialDenoisePixel(i, j, image_size_.x(), image_size_.y(), 8, gbuffer_, ping_pong_buffer_);
            }
        }).wait();
    }

    const std::vector<std::vector<Vector4>> &pass_input = config.spatial_denoise ? ping_pong_buffer_ : gbuffer_.color;

    auto moving_average = static_cast<float>(cumulated_sample_count_) / static_cast<float>(next_cumulative_sample_);

    // temporal denoise
    TaskManager::ParallelFor(0u, image_size_.y(), [this, &pass_input, moving_average](unsigned j) {
        for (auto i = 0u; i < image_size_.x(); i++)
        {
            const Vector4 &new_pixel = pass_input[j][i];

            auto &accumulated_pixel = frame_buffer_[j][i];

            accumulated_pixel = utilities::Lerp(new_pixel, accumulated_pixel, moving_average);
        }
    }).wait();

    [[unlikely]] if (debug_point.x() < image_size_.x() && debug_point.y() < image_size_.y())
    {
        Log(Info, "frame buffer {}", utilities::VectorToString(frame_buffer_[debug_point.y()][debug_point.x()]));
    }
}

void CameraRenderProxy::Update(RHIContext *rhi, const CameraRenderProxy &camera, const RenderConfig &config)
{
    RenderProxy::Update(rhi, camera, config);

    if (data_dirty_)
    {
        auto h = std::tan(state_.vertical_fov / 2.f);
        focus_plane_.height = 2.f * h * state_.focus_distance;
        focus_plane_.width = GetAspectRatio() * focus_plane_.height;

        sub_pixel_count_ = static_cast<unsigned>(std::lround(std::sqrt(static_cast<float>(config.sample_per_pixel))));
        actual_sample_per_pixel_ = sub_pixel_count_ * sub_pixel_count_;

        SetupProjectionMatrix();

        if (need_cpu_frame_buffer_)
        {
            gbuffer_.Resize(image_size_.x(), image_size_.y());
            ping_pong_buffer_.resize(image_size_.y(), std::vector<Vector4>(image_size_.x()));
            frame_buffer_.resize(image_size_.y(), std::vector<Vector4>(image_size_.x()));
        }
        else
        {
            gbuffer_.Clear();
            ping_pong_buffer_.clear();
            frame_buffer_.clear();
        }

        transform_dirty_ = true;
        pixels_dirty_ = true;
        data_dirty_ = false;
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
        next_cumulative_sample_ = 0;
    }

    cumulated_sample_count_ = std::min(static_cast<unsigned>(config.max_sample_per_pixel), next_cumulative_sample_);
    next_cumulative_sample_ = cumulated_sample_count_ + pending_sample_count_;

    pending_sample_count_ = 0;
}

void CameraRenderProxy::RenderCPU(const SceneRenderProxy &scene, const RenderConfig &config,
                                  const Vector2UInt &debug_point)
{
    PROFILE_SCOPE("camera render cpu");

    if (NeedClear())
    {
        ClearPixels();
    }

    BasePass(scene, config, debug_point);

    DenoisePass(config, debug_point);

    AccumulateSample(actual_sample_per_pixel_);

    total_frame_++;
}

void CameraRenderProxy::Print(Image2D &image)
{
    TaskManager::ParallelFor(0u, image_size_.y(), [&image, this](unsigned j) {
        for (auto i = 0u; i < image_size_.x(); i++)
        {
            const Vector3 &pixel = ToneMapping(frame_buffer_[j][i].head<3>());
            image.SetPixel(i, image_size_.y() - 1 - j, pixel);
        }
    }).wait();
}

static Vector3 ACESFilm(const Vector3 &hdr_color, float exposure)
{
    Scalar a = 2.51f;
    Scalar b = 0.03f;
    Scalar c = 2.43f;
    Scalar d = 0.59f;
    Scalar e = 0.14f;
    auto color = hdr_color.array() * exposure;
    return utilities::Clamp((color * (color * a + b)) / (color * (color * c + d) + e), 0, 1);
}

Vector3 CameraRenderProxy::ToneMapping(const Vector3 &color) const
{
    return ACESFilm(color, state_.exposure);
}

void CameraRenderProxy::SetupViewRay(Ray &ray, float u, float v) const
{
    // lens plane: centered at position_ and perpendicular to front_
    // image plane: (u, v)
    // focus plane: (u * max_u, v * max_v)
    // pixels on image plane are one-to-one mapped to focus plane

    // use a noise at lens plane to simulate aperture
    const Vector2 aperture_noise = sampler::UnitDisk() * state_.aperture_radius;
    const Vector3 lens_offset = aperture_noise.x() * right_ + aperture_noise.y() * up_;

    const Vector3 ray_origin = position_ + lens_offset;
    const Vector3 location_on_focus_plane = focus_plane_.lower_left + u * focus_plane_.max_u + v * focus_plane_.max_v;
    const Vector3 ray_direction = (location_on_focus_plane - ray_origin).normalized();

    ray.Reset(ray_origin, ray_direction);
}

void CameraRenderProxy::ClearPixels()
{
    ASSERT(pixels_dirty_);

    if (need_cpu_frame_buffer_)
    {
        TaskManager::ParallelFor(0u, image_size_.y(), [this](unsigned j) {
            auto &row = frame_buffer_[j];
            std::ranges::fill(row, Vector4::Zero());
        }).wait();
    }

    pixels_dirty_ = false;
}

void CameraRenderProxy::SetupProjectionMatrix()
{
    const float theta = state_.vertical_fov * 0.5f;
    const float inv_range = 1.f / (far_ - near_);
    const float inv_tan = 1.f / std::tan(theta);

    projection_matrix_.setZero();
    projection_matrix_(0, 0) = inv_tan / GetAspectRatio();
    projection_matrix_(1, 1) = -inv_tan;
    projection_matrix_(2, 2) = -far_ * inv_range;
    projection_matrix_(2, 3) = -near_ * far_ * inv_range;
    projection_matrix_(3, 2) = -1.f;
    projection_matrix_(3, 3) = 0;
}

void CameraRenderProxy::InitRenderResources(RHIContext *rhi, const RenderConfig &config)
{
    RenderProxy::InitRenderResources(rhi, config);

    // TODO(tqjxlm): handle resizing
    image_size_ = {config.image_width, config.image_height};

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
    position_ = transform.GetTranslation();

    transform.ExtractLocalBasis(right_, front_, up_);

    ASSERT(state_.focus_distance > 0);

    focus_plane_.max_u = right_ * focus_plane_.width;
    focus_plane_.max_v = up_ * focus_plane_.height;

    focus_plane_.lower_left =
        position_ + front_ * state_.focus_distance - focus_plane_.max_u * .5f - focus_plane_.max_v * .5f;

    view_matrix_ =
        utilities::ZUpToYUpMatrix() * (transform.GetRotation() * Eigen::Translation<Scalar, 3>(-position_)).matrix();

    view_projection_matrix_ = projection_matrix_ * view_matrix_;

    pixels_dirty_ = true;
}
} // namespace sparkle
