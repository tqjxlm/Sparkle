#pragma once

#include "core/math/Sampler.h"
#include "core/math/Utilities.h"

namespace sparkle
{
struct SampleResult
{
    bool is_valid = false;
    Vector3 throughput;
    Vector3 local_w_i;
};

struct SurfaceAttribute
{
    Vector3 normal;
    Vector3 tangent;
    Vector3 base_color;
    float roughness;
    float metallic;
    float eta = 1.f;
};

class BxDF
{
public:
    // notice: BSDF assumes tangent space, i.e. the local surface normal always points to (0, 0, 1)
    // use TransformBasisToWorld and TransformBasisToLocal to transform light directions
    // [[nodiscard]] virtual Vector3 Evaluate(const Vector3 &base_color, const Vector3 &w_o, Vector3 &w_i,
    //                                        float &probability) const = 0;
};

class LambertianBxDF : public BxDF
{
public:
    static SampleResult Sample(const Vector3 & /*w_o*/, const SurfaceAttribute &surface)
    {
        SampleResult result;
        result.is_valid = true;
        auto local_w_i = sampler::CosineWeightedHemiSphere::Sample();
        result.local_w_i = local_w_i;

        // float probability = sampler::CosineWeightedHemiSphere::Pdf(local_w_i);
        // result.throughput = surface.base_color * InvPi / probability * utilities::SaturatedCosTheta(local_w_i);
        result.throughput = surface.base_color;

        return result;
    }
};

class SpecularBxDF : public BxDF
{
public:
    static SampleResult Sample(const Vector3 &local_w_o, const SurfaceAttribute &surface)
    {
        SampleResult result;

        // enter tangent space

        const auto &local_w_m = sampler::SampleMicroFacetNormal(local_w_o, surface.roughness);
        Vector3 local_w_i = utilities::Reflect(local_w_o, local_w_m);

        Vector3 fresnel_color;
        Vector3 throughput;

        auto cos_o = utilities::SaturatedCosTheta(local_w_o);
        auto cos_i = utilities::SaturatedCosTheta(local_w_i);
        auto cos_m = utilities::SaturatedCosTheta(local_w_m);
        auto local_cos_i = local_w_i.dot(local_w_m);

        if (cos_o > Eps && cos_i > Eps && cos_m > Eps)
        {
            // we want to sample only one light per run, even if the surface is hybrid.
            // by doing this, there's no need to account for metallic term later.
            if (sampler::RandomUnit() < surface.metallic)
            {
                // if metal, all light is reflected

                // fresnel tells how reflected light lerps between color of the metal or color of the incoming light
                fresnel_color = utilities::SchlickApproximation(local_cos_i, surface.base_color);
                result.is_valid = true;
            }
            else
            {
                // if non-metal, fresnel tells how much light is reflected

                // F0: reflection rate when view direction is parallel to the surface (fully reflective).
                // this value is empirical and widely adopted.
                static const Scalar F0 = 0.04f;
                auto fresnel = utilities::SchlickApproximation(local_cos_i, F0);

                // we want to sample only one light per run, even if specular and diffuse reflection coexists.
                // by doing this, there's no need to account for ks and kd later.
                if (sampler::RandomUnit() < fresnel)
                {
                    fresnel_color = Ones;
                    result.is_valid = true;
                }
            }
        }

        if (!result.is_valid)
        {
            return result;
        }

        // auto occlusion = utilities::GeometrySmith(cos_o, cos_i, roughness);
        // auto normalizer = cos_m * cos_o + Eps;
        // throughput = fresnel_color * occlusion / normalizer * utilities::Saturate(local_cos_i);

        auto occlusion = utilities::SmithGGXCorrelated(cos_o, cos_i, surface.roughness);
        auto normalizer = utilities::GeometrySchlickGGX(cos_o, surface.roughness) + Eps;
        result.throughput = fresnel_color * occlusion / normalizer;
        result.local_w_i = local_w_i;

        return result;
    }
};

class DieletricBxDF : public BxDF
{
public:
    static SampleResult Sample(const Vector3 &local_w_o, const SurfaceAttribute &surface)
    {
        SampleResult result;

        auto fresnel = utilities::FrDielectric(utilities::CosTheta(local_w_o), EtaI, surface.eta);

        result.is_valid = true;
        if (sampler::RandomUnit() < fresnel)
        {
            result.local_w_i = utilities::Reflect(local_w_o);
        }
        else
        {
            const bool is_entering = utilities::CosTheta(local_w_o) > 0;
            const float eta_i_over_eta_t = is_entering ? EtaI / surface.eta : surface.eta / EtaI;
            result.local_w_i = utilities::Refract(local_w_o, eta_i_over_eta_t);
        }

        result.throughput = surface.base_color;

        return result;
    }

private:
    // use air as "outside"
    constexpr static float EtaI = 1.0f;
};
} // namespace sparkle
