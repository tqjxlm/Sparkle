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
        SurfaceAttribute surface{.normal = normal,
                                 .tangent = tangent,
                                 .base_color = GetBaseColor(uv),
                                 .roughness = GetRoughness(uv),
                                 .metallic = GetMetallic(uv)};

        // enter tangent space
        Vector3 w_o = -ray.Direction();
        const Vector3 &local_w_o = utilities::TransformBasisToLocal(w_o, normal, tangent);

        const auto sample = PbrBxDF::Sample(local_w_o, surface);

        // back to world space
        w_i = utilities::TransformBasisToWorld(sample.local_w_i, normal, tangent).normalized();

        return sample.throughput;
    }
};
} // namespace sparkle
