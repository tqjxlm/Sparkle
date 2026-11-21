#include "renderer/proxy/SkyRenderProxy.h"

#include "core/math/AliasTable.h"
#include "core/math/Ray.h"
#include "io/Image.h"
#include "renderer/RenderConfig.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/resource/ImageBasedLighting.h"
#include "rhi/RHI.h"

namespace sparkle
{
// a coarser table than the sky map: light sampling only needs where the light is, and MIS covers the rest
static constexpr unsigned MaxSamplingResolution = 128;

// the solid angle a cube face spans from its center to (u, v), with the face over [-1, 1]^2 at distance 1
static float CubeFaceSolidAngle(float u, float v)
{
    return std::atan2(u * v, std::sqrt(u * u + v * v + 1.f));
}

SkyRenderProxy::SkyRenderProxy(std::shared_ptr<const Image2DCube> sky_map) : sky_map_raw_(std::move(sky_map))
{
}

SkyRenderProxy::~SkyRenderProxy() = default;

void SkyRenderProxy::InitRenderResources(RHIContext *rhi, const RenderConfig &config)
{
    LightRenderProxy::InitRenderResources(rhi, config);

    sky_map_ = nullptr;
    image_based_lighting_.reset();
    sampling_table_ = nullptr;
    sampling_resolution_ = 0;

    if (sky_map_raw_)
    {
        sky_map_ = rhi->CreateTextureCube(sky_map_raw_.get(), sky_map_raw_->GetName());

        const bool raster_ibl =
            config.pipeline == RenderConfig::Pipeline::Forward || config.pipeline == RenderConfig::Pipeline::Deferred;
        if (raster_ibl && (config.use_diffuse_ibl || config.use_specular_ibl))
        {
            image_based_lighting_ = std::make_unique<ImageBasedLighting>(sky_map_, sky_map_raw_);
            image_based_lighting_->InitRenderResources(rhi, config);
        }

        if (config.pipeline == RenderConfig::Pipeline::Gpu)
        {
            CreateSamplingTable(rhi);
        }
    }

    ubo_.has_sky_map = sky_map_ ? 1 : 0;
}

void SkyRenderProxy::CreateSamplingTable(RHIContext *rhi)
{
    const unsigned resolution = std::min(sky_map_raw_->GetWidth(), MaxSamplingResolution);
    const unsigned sub_samples = std::clamp(sky_map_raw_->GetWidth() / resolution, 1u, 4u);
    const float texel_size = 2.f / static_cast<float>(resolution);

    // each texel weighs its luminance by its solid angle
    std::vector<float> weights;
    weights.reserve(static_cast<size_t>(Image2DCube::FaceId::Count) * resolution * resolution);
    for (uint8_t face_id = 0; face_id < Image2DCube::FaceId::Count; face_id++)
    {
        const auto &face = sky_map_raw_->GetFace(static_cast<Image2DCube::FaceId>(face_id));
        for (unsigned y = 0; y < resolution; y++)
        {
            for (unsigned x = 0; x < resolution; x++)
            {
                Vector3 radiance = Zeros;
                for (unsigned sub_y = 0; sub_y < sub_samples; sub_y++)
                {
                    for (unsigned sub_x = 0; sub_x < sub_samples; sub_x++)
                    {
                        const Vector2 sub_texel{(static_cast<float>(sub_x) + 0.5f) / static_cast<float>(sub_samples),
                                                (static_cast<float>(sub_y) + 0.5f) / static_cast<float>(sub_samples)};
                        const Vector3 sample =
                            face.Sample((Vector2(static_cast<float>(x), static_cast<float>(y)) + sub_texel) /
                                        static_cast<float>(resolution));
                        radiance += sample.cwiseMax(Zeros).cwiseMin(Ones * CameraRenderProxy::OutputLimit);
                    }
                }

                const float u0 = static_cast<float>(x) * texel_size - 1.f;
                const float v0 = static_cast<float>(y) * texel_size - 1.f;
                const float u1 = u0 + texel_size;
                const float v1 = v0 + texel_size;
                const float solid_angle = CubeFaceSolidAngle(u1, v1) - CubeFaceSolidAngle(u0, v1) -
                                          CubeFaceSolidAngle(u1, v0) + CubeFaceSolidAngle(u0, v0);

                weights.push_back(std::max(0.f, utilities::Luminance(radiance)) * solid_angle);
            }
        }
    }

    const auto table = BuildAliasTable(weights);
    if (table.empty())
    {
        return;
    }

    sampling_table_ =
        rhi->CreateBuffer({.size = sizeof(AliasTableEntry) * table.size(),
                           .usages = RHIBuffer::BufferUsage::StorageBuffer,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "SkySamplingTable");
    sampling_table_->UploadImmediate(table.data());
    sampling_resolution_ = resolution;
}

Vector3 SkyRenderProxy::Evaluate(const Ray &ray) const
{
    auto d = ray.Direction();
    [[likely]] if (sky_map_)
    {
        auto sample = sky_map_raw_->Sample(d);
        return sample.cwiseMax(Zeros).cwiseMin(Ones * CameraRenderProxy::OutputLimit);
    }

    // top: color. bottom: white.
    const float t = 0.5f * d.z() + 0.5f;
    auto sky_light = utilities::Lerp(Ones, ubo_.color, t);

    return sky_light;
}
} // namespace sparkle
