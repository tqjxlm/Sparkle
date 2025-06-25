#include "renderer/renderer/CPURenderer.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "core/TaskManager.h"
#include "core/math/Intersection.h"
#include "core/math/Ray.h"
#include "core/math/Sampler.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/pass/UiPass.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/MaterialRenderProxy.h"
#include "renderer/proxy/PrimitiveRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
CPURenderer::CPURenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                         SceneRenderProxy *scene_render_proxy)
    : Renderer(render_config, rhi_context, scene_render_proxy),
      output_image_(image_size_.x(), image_size_.y(), PixelFormat::RGBAFloat16)
{
    ASSERT_EQUAL(render_config.pipeline, RenderConfig::Pipeline::cpu);
}

CPURenderer::~CPURenderer() = default;

void CPURenderer::InitRenderResources()
{
    scene_render_proxy_->InitRenderResources(rhi_, render_config_);

    camera_ = scene_render_proxy_->GetCamera();

    image_buffer_ = rhi_->CreateBuffer({.size = output_image_.GetStorageSize(),
                                        .usages = RHIBuffer::BufferUsage::TransferSrc,
                                        .mem_properties = RHIMemoryProperty::None,
                                        .is_dynamic = true},
                                       "RayTracingOutputBuffer");

    screen_texture_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = output_image_.GetFormat(),
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                        .filtering_method_min = RHISampler::FilteringMethod::Linear,
                        .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Linear},
            .width = image_size_.x(),
            .height = image_size_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::TransferDst |
                      RHIImage::ImageUsage::ColorAttachment,
            .msaa_samples = static_cast<uint8_t>(rhi_->GetConfig().msaa_samples),
        },
        "CpuPipelineColorBuffer");

    auto base_rt = rhi_->CreateRenderTarget({}, screen_texture_, nullptr, "CpuPipelineRenderTarget");

    screen_quad_pass_ =
        PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, screen_texture_, rhi_->GetBackBufferRenderTarget());

    ui_pass_ = PipelinePass::Create<UiPass>(render_config_, rhi_, base_rt);

    gbuffer_.Resize(image_size_.x(), image_size_.y());
    ping_pong_buffer_.resize(image_size_.y(), std::vector<Vector4>(image_size_.x()));
    frame_buffer_.resize(image_size_.y(), std::vector<Vector4>(image_size_.x()));

    sub_pixel_count_ =
        static_cast<unsigned>(std::lround(std::sqrt(static_cast<float>(render_config_.sample_per_pixel))));
    actual_sample_per_pixel_ = sub_pixel_count_ * sub_pixel_count_;
}

void CPURenderer::Update()
{
    PROFILE_SCOPE("CPURenderer::Update");

    screen_quad_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    if (ui_pass_)
    {
        ui_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }
}

void CPURenderer::Render()
{
    PROFILE_SCOPE("CPURenderer::Render");

    // CPU workload: software ray tracing
    {
        if (camera_->NeedClear())
        {
            TaskManager::ParallelFor(0u, image_size_.y(), [this](unsigned j) {
                auto &row = frame_buffer_[j];
                std::ranges::fill(row, Vector4::Zero());
            }).wait();

            camera_->ClearPixels();
        }

        BasePass(*scene_render_proxy_, render_config_, debug_point_);

        DenoisePass(render_config_, debug_point_);

        ToneMappingPass(output_image_);
    }

    // GPU workload: copy the image to a texture
    {
        image_buffer_->Upload(rhi_, output_image_.GetRawData());

        screen_texture_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                     .after_stage = RHIPipelineStage::Top,
                                     .before_stage = RHIPipelineStage::Transfer});

        image_buffer_->CopyToImage(screen_texture_.get());
    }

    // post process: ui
    {
        if (render_config_.render_ui)
        {
            screen_texture_->Transition({.target_layout = RHIImageLayout::ColorOutput,
                                         .after_stage = RHIPipelineStage::Transfer,
                                         .before_stage = RHIPipelineStage::ColorOutput});
            ui_pass_->Render();
            screen_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                         .after_stage = RHIPipelineStage::ColorOutput,
                                         .before_stage = RHIPipelineStage::PixelShader});
        }
        else
        {
            screen_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                         .after_stage = RHIPipelineStage::Transfer,
                                         .before_stage = RHIPipelineStage::PixelShader});
        }
    }

    // screen pass: render it on a screen quad
    {
        screen_quad_pass_->Render();
    }

    camera_->AccumulateSample(actual_sample_per_pixel_);
}

struct SampleResult
{
    Vector3 color = Zeros;
    Vector3 world_normal = Zeros;
    float valid_flag = 1.f;
};

static void SetupViewRay(CameraRenderProxy *camera, Ray &ray, float u, float v)
{
    // lens plane: centered at position_ and perpendicular to front_
    // image plane: (u, v)
    // focus plane: (u * max_u, v * max_v)
    // pixels on image plane are one-to-one mapped to focus plane

    // use a noise at lens plane to simulate aperture
    const Vector2 aperture_noise = sampler::UnitDisk() * camera->GetAttribute().aperture_radius;
    const Vector3 lens_offset =
        aperture_noise.x() * camera->GetPosture().right + aperture_noise.y() * camera->GetPosture().up;

    const Vector3 ray_origin = camera->GetPosture().position + lens_offset;
    const Vector3 location_on_focus_plane =
        camera->GetFocusPlane().lower_left + u * camera->GetFocusPlane().max_u + v * camera->GetFocusPlane().max_v;
    const Vector3 ray_direction = (location_on_focus_plane - ray_origin).normalized();

    ray.Reset(ray_origin, ray_direction);
}

static SampleResult SamplePixel(const SceneRenderProxy &scene, const RenderConfig &config, CameraRenderProxy *camera,
                                float u, float v, bool debug)
{
    Ray ray(debug);
    SetupViewRay(camera, ray, u, v);

    Vector3 throughput = Ones;
    Intersection intersection;

    SampleResult result;

    const auto *sky_light = scene.GetSkyLight();

    Vector3 debug_color = Zeros;

    auto camera_posture = camera->GetPosture();

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
            result.color = Ones * (intersection.GetLocation() - camera_posture.position).dot(camera_posture.front) /
                           camera->GetFar();
            return result;
        [[likely]] default:
            break;
        }

        // core procedure: radiance decay every bounce
        throughput = throughput.cwiseProduct(this_throughput);

        [[unlikely]] if (ray.IsDebug())
        {
            Log(Warn, "Hit bounce {}. This throughput {}. Throughput {}. Next direction {}", bounce,
                utilities::VectorToString(this_throughput), utilities::VectorToString(throughput),
                utilities::VectorToString(next_direction));
            ray.Print();
            intersection.Print();
            material->PrintSample(tex_coord);
        }

        // terminal condition: early out
        if (throughput.squaredNorm() < Eps)
        {
            result.valid_flag = -1.f;
            break;
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

void CPURenderer::RenderPixel(unsigned i, unsigned j, Scalar pixel_width, Scalar pixel_height,
                              const SceneRenderProxy &scene, const RenderConfig &config, const Vector2UInt &debug_point)
{
    const bool is_debug = i == debug_point.x() && j == debug_point.y();

    auto u = (static_cast<float>(i) + sampler::RandomUnit()) * pixel_width;
    auto v = (static_cast<float>(j) + sampler::RandomUnit()) * pixel_height;

    auto result = SamplePixel(scene, config, camera_, u, v, is_debug);

    result.color = result.color.cwiseMin(Ones * CameraRenderProxy::OutputLimit);

    gbuffer_.color[j][i].head<3>() = result.color;
    gbuffer_.color[j][i].w() = result.valid_flag;

    if (config.debug_mode == RenderConfig::DebugMode::Color && config.spatial_denoise)
    {
        gbuffer_.world_normal[j][i] = result.world_normal;
    }
}

void CPURenderer::BasePass(const SceneRenderProxy &scene, const RenderConfig &config, const Vector2UInt &debug_point)
{
    PROFILE_SCOPE("CPURenderer base pass");

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
                                const CPUGBuffer &gbuffer, std::vector<std::vector<Vector4>> &output_buffer)
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

        // this channel is reserved for future use
        output_buffer[new_j][new_i].w() = 1.f;

        break;
    }
}

void CPURenderer::DenoisePass(const RenderConfig &config, const Vector2UInt &debug_point)
{
    PROFILE_SCOPE("CPURenderer denoise pass");

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

    auto cumulated_sample_count = camera_->GetCumulatedSampleCount();
    auto moving_average = static_cast<float>(cumulated_sample_count) /
                          static_cast<float>(cumulated_sample_count + actual_sample_per_pixel_);

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
        Log(Info, "frame buffer {}. new pixel {}",
            utilities::VectorToString(frame_buffer_[debug_point.y()][debug_point.x()]),
            utilities::VectorToString(pass_input[debug_point.y()][debug_point.x()]));
    }
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

void CPURenderer::ToneMappingPass(Image2D &image)
{
    PROFILE_SCOPE("CPURenderer tonemapping pass");

    TaskManager::ParallelFor(0u, image_size_.y(), [&image, this](unsigned j) {
        for (auto i = 0u; i < image_size_.x(); i++)
        {
            const Vector3 &pixel = ACESFilm(frame_buffer_[j][i].head<3>(), camera_->GetAttribute().exposure);
            image.SetPixel(i, image_size_.y() - 1 - j, pixel);
        }
    }).wait();
}
} // namespace sparkle
