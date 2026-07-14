#include "renderer/resource/IblBrdfCookJob.h"

#include "core/math/Types.h"
#include "core/task/TaskManager.h"
#include "renderer/resource/IblCookMath.h"

#include <cstring>

namespace sparkle
{
CookJobResult IblBrdfCookJob::Execute()
{
    using ibl_cook::GeometrySchlickGGX;
    using ibl_cook::Reflect;
    using ibl_cook::SampleMicroFacetNormal;
    using ibl_cook::SaturateDot;
    using ibl_cook::ShaderEps;
    using ibl_cook::ShaderRandom;
    using ibl_cook::SmithGGX;

    constexpr unsigned Resolution = MapSize;
    constexpr size_t PixelSize = sizeof(Vector4h);

    std::vector<char> payload(static_cast<size_t>(Resolution) * Resolution * PixelSize);

    TaskManager::ParallelFor(0u, Resolution, [this, &payload](unsigned y) {
        const float pixel_size = 1.f / (static_cast<float>(Resolution) - 1.f);

        for (unsigned x = 0; x < Resolution; x++)
        {
            const float cos_o = static_cast<float>(x) * pixel_size;
            const float roughness = static_cast<float>(y) * pixel_size;

            const Vector3 local_w_o(std::sqrt(std::max(0.f, 1.f - cos_o * cos_o)), 0.f, cos_o);

            Eigen::Vector2f total = Eigen::Vector2f::Zero();

            for (unsigned sample_index = 0; sample_index < TargetSampleCount; sample_index++)
            {
                ShaderRandom rng(x, y, 1u + sample_index);

                Vector3 local_w_m = SampleMicroFacetNormal(rng, local_w_o, roughness);
                Vector3 local_w_i = Reflect(-local_w_o, local_w_m);
                float i_dot_m = SaturateDot(local_w_m, local_w_i);

                float cos_i = local_w_i.z();

                if (cos_i >= ShaderEps)
                {
                    float geometry = SmithGGX(cos_o, cos_i, roughness);
                    float normalizer = GeometrySchlickGGX(i_dot_m, roughness) + ShaderEps;
                    float brdf_term = geometry / normalizer;
                    float fresnel_term = std::pow(std::max(0.f, 1.f - i_dot_m), 5.f);
                    total += Eigen::Vector2f((1.f - fresnel_term) * brdf_term, fresnel_term * brdf_term);
                }
            }

            const Vector4 color(total.x() / TargetSampleCount, total.y() / TargetSampleCount, 0.f,
                                static_cast<float>(TargetSampleCount));
            const Vector4h half_color = color.cast<Half>();

            std::memcpy(payload.data() + (static_cast<size_t>(y) * Resolution + x) * PixelSize, half_color.data(),
                        PixelSize);
        }

        cooked_rows_++;
    }).wait();

    return CookJobResult::Success(std::move(payload));
}
} // namespace sparkle
