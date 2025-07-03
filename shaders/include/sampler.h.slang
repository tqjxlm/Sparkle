#ifndef SAMPLER_H_
#define SAMPLER_H_

#include "math.h.slang"
#include "random.h.slang"
#include "tangent_space.h.slang"

namespace DistributionGGX
{
float Evaluate(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = Pi * denom * denom;

    return num / denom;
}

float3 Sample(float roughness)
{
    float eta_1 = RandomUnit();
    float eta_2 = RandomUnit();

    float a = roughness * roughness;
    float a2 = a * a;

    float theta = acos(sqrt((1.f - eta_1) / ((a2 - 1.f) * eta_1 + 1.f)));
    float cos_theta = cos(theta);
    float sin_theta = sin(theta);

    float phi = 2.f * Pi * eta_2;
    float sin_phi = sin(phi);
    float cos_phi = cos(phi);

    return float3(cos_phi * sin_theta, sin_phi * sin_theta, cos_theta);
}

float Ndf(float cos_theta, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;

    float d = (a2 - 1.f) * cos_theta * cos_theta + 1.f;

    return a2 / (Pi * d * d);
}

float Pdf(float3 w_m, float roughness)
{
    float cos_theta = CosTheta(w_m);

    return cos_theta * Ndf(cos_theta, roughness);
}
} // namespace DistributionGGX

float GeometrySchlickGGX(float cos_theta, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;

    float tan2_v = (1.f - cos_theta * cos_theta) / (cos_theta * cos_theta + Eps);
    return 2.f / (1.f + sqrt(1.f + a2 * tan2_v));
}

namespace DistributionVn
{
float3 Sample(float3 w_o, float roughness)
{
    float a = roughness * roughness;

    float u1 = RandomUnit();
    float u2 = RandomUnit();

    float3 v = normalize(float3(w_o.x * a, w_o.y * a, w_o.z));

    float3 t1 = (v.z < 0.999f) ? normalize(cross(v, float3(0, 0, 1))) : float3(1, 0, 0);
    float3 t2 = cross(v, t1);

    float r = sqrt(u1);
    float phi = 2.f * Pi * u2;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5f * (1.f + v.z);
    p2 = (1.f - s) * sqrt(1.f - p1 * p1) + s * p2;

    float3 n = p1 * t1 + p2 * t2 + sqrt(max(0.f, 1.f - p1 * p1 - p2 * p2)) * v;

    return normalize(float3(a * n.x, a * n.y, max(0.f, n.z)));
}

float Pdf(float roughness, float3 w_o, float3 w_m)
{
    float G = GeometrySchlickGGX(w_o.z, roughness);
    float D = DistributionGGX::Ndf(w_m.z, roughness);
    return G * D * SaturateDot(w_o, w_m) / (w_o.z + Eps);
}
} // namespace DistributionVn

float SmithGGX(float cos_o, float cos_i, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;

    float ggx_i = cos_o * sqrt((-cos_i * a2 + cos_i) * cos_i + a2 + Eps);
    float ggx_o = cos_i * sqrt((-cos_o * a2 + cos_o) * cos_o + a2 + Eps);
    return 2.f * cos_o * cos_i / (ggx_o + ggx_i);

    // another implementation

    // float ggx2 = GeometrySchlickGGX(cos_o, roughness);
    // float ggx1 = GeometrySchlickGGX(cos_i, roughness);

    // return ggx1 * ggx2;
}

float3 FresnelSchlickRoughness(float cos_theta_i, float3 r0, float roughness)
{
    return r0 + (max((float3)(1.0 - roughness), r0) - r0) * pow(saturate(1.0 - cos_theta_i), 5.0);
}

float FresnelSchlick(float cos_theta_i, float r0)
{
    return lerp(r0, 1.0, pow(saturate(1.f - cos_theta_i), 5.f));
}

float3 FresnelSchlick(float cos_theta_i, float3 r0)
{
    return lerp(r0, (float3)1, pow(saturate(1.f - cos_theta_i), 5.f));
}

float FresnelDielectric(float cos_theta_i, float eta_i, float eta_t)
{
    bool entering = cos_theta_i > 0.f;
    if (!entering)
    {
        float tmp = eta_i;
        eta_i = eta_t;
        eta_t = tmp;
        cos_theta_i = -cos_theta_i;
    }

    float sin_theta_i = sqrt(1.f - cos_theta_i * cos_theta_i);
    float ref_idx = eta_i / eta_t;
    float sin_theta_t = ref_idx * sin_theta_i;

    if (sin_theta_t >= 1.f)
    {
        return 1.f;
    }

    float r0 = (1.f - ref_idx) / (1.f + ref_idx);
    r0 = r0 * r0;
    return FresnelSchlick(cos_theta_i, r0);
}

float3 SampleMicroFacetNormal(float roughness)
{
    return DistributionGGX::Sample(roughness);
}

float3 SampleMicroFacetNormal(float3 w_o, float roughness)
{
    return DistributionVn::Sample(w_o, roughness);
}

#endif // SAMPLER_H_