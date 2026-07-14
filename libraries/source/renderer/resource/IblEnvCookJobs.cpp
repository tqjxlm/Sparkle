#include "renderer/resource/IblEnvCookJobs.h"

#include "core/Exception.h"
#include "core/task/TaskManager.h"
#include "io/Image.h"
#include "renderer/resource/IblCookMath.h"

#include <cstring>

namespace sparkle
{
IblEnvCookJob::IblEnvCookJob(std::shared_ptr<const Image2DCube> env_map) : env_map_(std::move(env_map))
{
    ASSERT(env_map_->GetWidth() == env_map_->GetHeight());

    env_hash_ = env_map_->GetContentHash();
}

std::string IblEnvCookJob::GetSourceName() const
{
    return env_map_->GetName();
}

CookJobResult IblDiffuseCookJob::Execute()
{
    using ibl_cook::ClampToLength;
    using ibl_cook::CosineWeightedHemisphereSample;
    using ibl_cook::GetDirectionFromCubeMapUV;
    using ibl_cook::GetLocalAxisFromNormal;
    using ibl_cook::SampleCubeHardware;
    using ibl_cook::ShaderRandom;
    using ibl_cook::TransformBasisToWorld;

    constexpr unsigned Resolution = MapSize;
    constexpr size_t PixelSize = sizeof(Vector4h);
    const size_t face_size = static_cast<size_t>(Resolution) * Resolution * PixelSize;

    std::vector<char> payload(6 * face_size);

    TaskManager::ParallelFor(0u, 6 * Resolution, [this, &payload](unsigned row_index) {
        const unsigned face_id = row_index / Resolution;
        const unsigned y = row_index % Resolution;

        const float pixel_size = 1.f / (static_cast<float>(Resolution) - 1.f);

        for (unsigned x = 0; x < Resolution; x++)
        {
            const Vector3 w_n = GetDirectionFromCubeMapUV(static_cast<float>(x) * pixel_size,
                                                          static_cast<float>(y) * pixel_size, face_id);

            Vector3 u;
            Vector3 v;
            Vector3 w;
            GetLocalAxisFromNormal(w_n, u, v, w);

            Vector3 total = Zeros;

            for (unsigned sample_index = 0; sample_index < TargetSampleCount; sample_index++)
            {
                ShaderRandom rng(x, y, 1u + sample_index);

                Vector3 local_w_i = CosineWeightedHemisphereSample(rng);

                Vector3 w_i = TransformBasisToWorld(local_w_i, u, v, w).normalized();

                Vector3 env_sample =
                    ClampToLength(SampleCubeHardware(*env_map_, w_i), IblSettings::MaxEnvironmentBrightness);

                total += env_sample;
            }

            const Vector4 color(total.x() / TargetSampleCount, total.y() / TargetSampleCount,
                                total.z() / TargetSampleCount, static_cast<float>(TargetSampleCount));
            const Vector4h half_color = color.cast<Half>();

            std::memcpy(payload.data() + face_id * face_size + (static_cast<size_t>(y) * Resolution + x) * PixelSize,
                        half_color.data(), PixelSize);
        }

        cooked_rows_++;
    }).wait();

    return CookJobResult::Success(std::move(payload));
}

float IblSpecularCookJob::GetProgress() const
{
    unsigned total_rows = 0;
    for (uint8_t level = 0; level < MipLevelCount; level++)
    {
        total_rows += 6 * std::max(1u, MapSize >> level);
    }
    return static_cast<float>(cooked_rows_.load()) / static_cast<float>(total_rows);
}

CookJobResult IblSpecularCookJob::Execute()
{

    static constexpr size_t PixelSize = sizeof(Vector4h);

    size_t payload_size = 0;
    for (uint8_t level = 0; level < MipLevelCount; level++)
    {
        const auto res = static_cast<size_t>(std::max(1u, MapSize >> level));
        payload_size += 6 * res * res * PixelSize;
    }

    std::vector<char> payload(payload_size);

    size_t level_offset = 0;
    for (uint8_t level = 0; level < MipLevelCount; level++)
    {
        const auto resolution = std::max(1u, MapSize >> level);
        const size_t face_size = static_cast<size_t>(resolution) * resolution * PixelSize;
        const float roughness = 1.f / (MipLevelCount - 1) * level;

        TaskManager::ParallelFor(
            0u, 6 * resolution,
            [this, &payload, level_offset, face_size, resolution, roughness](unsigned row_index) {
                using ibl_cook::ClampToLength;
                using ibl_cook::GetDirectionFromCubeMapUV;
                using ibl_cook::SampleCubeHardware;
                using ibl_cook::GetLocalAxisFromNormal;
                using ibl_cook::Reflect;
                using ibl_cook::SampleMicroFacetNormal;
                using ibl_cook::ShaderEps;
                using ibl_cook::ShaderRandom;
                using ibl_cook::TransformBasisToWorld;

                const unsigned face_id = row_index / resolution;
                const unsigned y = row_index % resolution;

                const float pixel_size = 1.f / (static_cast<float>(resolution) - 1.f);

                const Vector3 local_w_o(0.f, 0.f, 1.f);

                for (unsigned x = 0; x < resolution; x++)
                {
                    const Vector3 w_n = GetDirectionFromCubeMapUV(static_cast<float>(x) * pixel_size,
                                                                  static_cast<float>(y) * pixel_size, face_id);

                    Vector3 u;
                    Vector3 v;
                    Vector3 w;
                    GetLocalAxisFromNormal(w_n, u, v, w);

                    Vector3 total = Zeros;

                    if (roughness * roughness < ShaderEps)
                    {
                        // SampleMicroFacetNormal returns the normal for a perfect mirror without
                        // consuming randomness, so every sample is this one value. accumulate the
                        // same way the shader does to keep float rounding identical.
                        Vector3 env_sample =
                            ClampToLength(SampleCubeHardware(*env_map_, w_n), IblSettings::MaxEnvironmentBrightness);
                        for (unsigned sample_index = 0; sample_index < TargetSampleCount; sample_index++)
                        {
                            total += env_sample;
                        }
                    }
                    else
                    {
                        for (unsigned sample_index = 0; sample_index < TargetSampleCount; sample_index++)
                        {
                            ShaderRandom rng(x, y, 1u + sample_index);

                            Vector3 local_w_m = SampleMicroFacetNormal(rng, local_w_o, roughness);
                            Vector3 local_w_i = Reflect(-local_w_o, local_w_m);

                            if (local_w_i.z() < ShaderEps)
                            {
                                continue;
                            }

                            Vector3 w_i = TransformBasisToWorld(local_w_i, u, v, w).normalized();

                            // the shader computes a pdf-based mip level here, but the environment map
                            // has a single mip so SampleLevel clamps to 0 on the GPU as well
                            Vector3 env_sample = ClampToLength(SampleCubeHardware(*env_map_, w_i),
                                                               IblSettings::MaxEnvironmentBrightness);

                            total += env_sample;
                        }
                    }

                    const Vector4 color(total.x() / TargetSampleCount, total.y() / TargetSampleCount,
                                        total.z() / TargetSampleCount, static_cast<float>(TargetSampleCount));
                    const Vector4h half_color = color.cast<Half>();

                    std::memcpy(payload.data() + level_offset + face_id * face_size +
                                    (static_cast<size_t>(y) * resolution + x) * PixelSize,
                                half_color.data(), PixelSize);
                }

                cooked_rows_++;
            })
            .wait();

        level_offset += 6 * face_size;
    }

    return CookJobResult::Success(std::move(payload));
}
} // namespace sparkle
