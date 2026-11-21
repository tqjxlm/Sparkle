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
};

// the metallic-roughness BSDF of SamplePbrBsdf in shaders/include/ray_trace.h.slang: a GGX specular lobe with Schlick
// fresnel, and a lambertian lobe for the light that the fresnel transmits into a dielectric
class PbrBxDF : public BxDF
{
public:
    // picks one lobe to sample from, and weights it by both lobes over their mixed pdf
    static SampleResult Sample(const Vector3 &local_w_o, const SurfaceAttribute &surface)
    {
        const Vector3 f0 = utilities::Lerp(Vector3(Ones * DielectricF0), surface.base_color, surface.metallic);

        SampleResult result;
        if (sampler::RandomUnit() < SpecularLobeProbability(local_w_o, surface, f0))
        {
            const float roughness = std::max(surface.roughness, MinRoughness);
            result.local_w_i = utilities::Reflect(local_w_o, sampler::SampleMicroFacetNormal(local_w_o, roughness));
        }
        else
        {
            result.local_w_i = sampler::CosineWeightedHemiSphere::Sample();
        }

        const auto [value, pdf] = Evaluate(local_w_o, result.local_w_i, surface, f0);
        result.is_valid = pdf > 0.f;
        result.throughput = result.is_valid ? Vector3(value / pdf) : Zeros;

        return result;
    }

private:
    // GGX roughness below this overflows the distribution's peak in fp32; it still renders as a mirror
    static constexpr float MinRoughness = 0.08f;

    static constexpr float DielectricF0 = 0.04f;

    static float SpecularLobeProbability(const Vector3 &local_w_o, const SurfaceAttribute &surface, const Vector3 &f0)
    {
        const auto specular = utilities::Luminance(utilities::SchlickApproximation(local_w_o.z(), f0));
        const auto diffuse = (1.f - surface.metallic) * utilities::Luminance(surface.base_color) * (1.f - specular);
        return specular / (specular + diffuse + Eps);
    }

    // f * cos_i of both lobes, and the pdf of Sample generating local_w_i
    static std::pair<Vector3, float> Evaluate(const Vector3 &local_w_o, const Vector3 &local_w_i,
                                              const SurfaceAttribute &surface, const Vector3 &f0)
    {
        const auto cos_o = local_w_o.z();
        const auto cos_i = local_w_i.z();
        if (cos_o <= Eps || cos_i <= Eps)
        {
            return {Zeros, 0.f};
        }

        const auto roughness = std::max(surface.roughness, MinRoughness);
        const Vector3 w_m = (local_w_o + local_w_i).normalized();
        const Vector3 fresnel = utilities::SchlickApproximation(local_w_o.dot(w_m), f0);
        const auto ndf = sampler::DistributionGGX::Ndf(w_m.z(), roughness);

        const Vector3 specular =
            fresnel * (ndf * utilities::SmithGGXCorrelated(cos_o, cos_i, roughness) / (4.f * cos_o));
        const Vector3 diffuse =
            (Ones - fresnel).cwiseProduct(surface.base_color) * ((1.f - surface.metallic) * InvPi * cos_i);

        // the VNDF pdf of w_m times the jacobian 1 / (4 * dot(w_o, w_m)) of the reflection
        const auto specular_pdf = utilities::GeometrySchlickGGX(cos_o, roughness) * ndf / (4.f * cos_o);
        const auto pdf = utilities::Lerp(cos_i * InvPi, specular_pdf, SpecularLobeProbability(local_w_o, surface, f0));

        return {specular + diffuse, pdf};
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
