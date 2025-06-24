#pragma once

#include "renderer/proxy/MaterialRenderProxy.h"

#include "core/math/Ray.h"
#include "scene/material/BxDF.h"

namespace sparkle
{
class PbrMaterialRenderProxy final : public MaterialRenderProxy
{
public:
    using MaterialRenderProxy::MaterialRenderProxy;

    Vector3 SampleSurface(const Ray &ray, Vector3 &w_i, const Vector3 &normal, const Vector3 &tangent,
                          const Vector2 &uv) const override
    {
        /*
            According to PBR theory, the outward light will present in 3 forms:
            1. (specular) metal reflection: controlled by metallic.
            2. (specular) dieletric reflection: controlled by fresnel.
            3. (diffuse) dieletric refraction: all remaining.

            Assumptions:
            1. light behaves deterministically when observed, which means outward light selects only one of the 3 forms.
            2. light is modeled as a vector with energy, rather than particles or waves as in quantum physics.
            3. no subsurface reflection, which means dieletric refraction behaves like diffuse reflection.
        */

        SurfaceAttribute surface{.normal = normal,
                                 .tangent = tangent,
                                 .base_color = GetBaseColor(uv),
                                 .roughness = GetRoughness(uv),
                                 .metallic = GetMetallic(uv)};

        // enter tangent space
        Vector3 w_o = -ray.Direction();
        const Vector3 &local_w_o = utilities::TransformBasisToLocal(w_o, normal, tangent);

        auto sample = SpecularBxDF::Sample(local_w_o, surface);

        if (!sample.is_valid)
        {
            sample = LambertianBxDF::Sample(local_w_o, surface);
        }

        // back to world space
        w_i = utilities::TransformBasisToWorld(sample.local_w_i, normal, tangent).normalized();

        return sample.throughput;
    }
};
} // namespace sparkle
